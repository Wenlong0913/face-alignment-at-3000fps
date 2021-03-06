#include <boost/python.hpp>
#ifdef _OPENMP
#include <omp.h>
#endif
#include <cmath>
#include <iostream>
#include "../lbf/liblinear/linear.h"
#include "../lbf/sampler.h"
#include "../lbf/randomforest/forest.h"
#include "trainer.h"

using std::cout;
using std::endl;
using std::flush;
using namespace lbf::randomforest;
namespace liblinear = lbf::liblinear;
namespace np = boost::python::numpy;

namespace lbf {
	namespace python {
		Trainer::Trainer(Corpus* training_corpus, Corpus* validation_corpus, Model* model, int augmentation_size, int num_features_to_sample){
			_training_corpus = training_corpus;
			_validation_corpus = validation_corpus;
			_model = model;
			_num_features_to_sample = num_features_to_sample;
			_augmentation_size = augmentation_size;

			std::cout << "augmentation_size = " << augmentation_size << std::endl;
			std::cout << "num_features_to_sample = " << num_features_to_sample << std::endl;

			int num_data = training_corpus->_images.size();
			_num_augmented_data = (_augmentation_size + 1) * num_data;

			// sample feature locations
			for(int stage = 0;stage < model->_num_stages;stage++){
				double localized_radius = model->_local_radius_at_stage[stage];

				std::vector<FeatureLocation> sampled_feature_locations;
				sampled_feature_locations.reserve(num_features_to_sample);

				for(int feature_index = 0;feature_index < num_features_to_sample;feature_index++){
					double r, theta;
					
					r = localized_radius * sampler::uniform(0, 1);
					theta = M_PI * 2.0 * sampler::uniform(0, 1);
					cv::Point2d a(r * std::cos(theta), r * std::sin(theta));
					
					r = localized_radius * sampler::uniform(0, 1);
					theta = M_PI * 2.0 * sampler::uniform(0, 1);
					cv::Point2d b(r * std::cos(theta), r * std::sin(theta));

					FeatureLocation location(a, b);
					sampled_feature_locations.push_back(location);
				}

				_sampled_feature_locations_at_stage.push_back(sampled_feature_locations);
			}

			// set initial shape
			int num_landmarks = model->_num_landmarks;

			_augmented_estimated_shapes.resize(_num_augmented_data);
			_augmented_target_shapes.resize(_num_augmented_data);
			_augmented_indices_to_data_index.resize(_num_augmented_data);

			// normalized shape
			for(int data_index = 0;data_index < num_data;data_index++){
				_augmented_estimated_shapes[data_index] = _model->_mean_shape.clone();	// make a copy
				_augmented_target_shapes[data_index] = training_corpus->get_normalized_shape(data_index);
				_augmented_indices_to_data_index[data_index] = data_index;
			}

			// augmented shapes
			for(int data_index = 0;data_index < num_data;data_index++){

				for(int n = 0;n < _augmentation_size;n++){
					int shape_index = 0;
					do {
						shape_index = sampler::uniform_int(0, num_data - 1);
					} while(shape_index == data_index);	// reject same shape
					int augmented_data_index = (n + 1) * num_data + data_index;
					_augmented_estimated_shapes[augmented_data_index] = training_corpus->get_normalized_shape(shape_index).clone();	// make a copy
					_augmented_target_shapes[augmented_data_index] = training_corpus->get_normalized_shape(data_index);
					_augmented_indices_to_data_index[augmented_data_index] = data_index;
				}
			}

		}
		cv::Mat1b & Trainer::get_image_by_augmented_index(int augmented_data_index){
			assert(augmented_data_index < _augmented_indices_to_data_index.size());
			int data_index = get_data_index_by_augmented_index(augmented_data_index);
			return _training_corpus->_images[data_index];
		}
		int Trainer::get_data_index_by_augmented_index(int augmented_data_index){
			assert(augmented_data_index < _augmented_indices_to_data_index.size());
			return _augmented_indices_to_data_index[augmented_data_index];
		}
		void Trainer::train(){
			for(int stage = 0;stage < _model->_num_stages;stage++){
				train_stage(stage);
			}
		}
		void Trainer::train_stage(int stage){
			cout << "training stage: " << (stage + 1) << " of " << _model->_num_stages << endl;

			// local binary features
			if(_model->_training_finished_at_stage[stage] == false){
				train_local_feature_mapping_functions(stage);
			}

			cout << "generating binary features ..." << endl;
			struct liblinear::feature_node** binary_features = new liblinear::feature_node*[_num_augmented_data];
			#pragma omp parallel for
			for(int augmented_data_index = 0;augmented_data_index < _num_augmented_data;augmented_data_index++){

				cv::Mat1b &image = get_image_by_augmented_index(augmented_data_index);
				cv::Mat1d projected_shape = project_current_estimated_shape(augmented_data_index);

				binary_features[augmented_data_index] = _model->compute_binary_features_at_stage(image, projected_shape, stage);
			}
			
			// global linear regression
			if(_model->_training_finished_at_stage[stage] == false){
				train_global_linear_regression_at_stage(stage, binary_features);
			}
			
			_model->finish_training_at_stage(stage);
				
			// predict shape
			for(int landmark_index = 0;landmark_index < _model->_num_landmarks;landmark_index++){

				struct liblinear::model* model_x = _model->get_linear_model_x_at(stage, landmark_index);
				struct liblinear::model* model_y = _model->get_linear_model_y_at(stage, landmark_index);

				for(int augmented_data_index = 0;augmented_data_index < _num_augmented_data;augmented_data_index++){
					cv::Mat1d &estimated_shape = _augmented_estimated_shapes[augmented_data_index];
					assert(estimated_shape.rows == _model->_num_landmarks && estimated_shape.cols == 2);

					double delta_x = liblinear::predict(model_x, binary_features[augmented_data_index]);
					double delta_y = liblinear::predict(model_y, binary_features[augmented_data_index]);

					// update shape
					estimated_shape(landmark_index, 0) += delta_x;
					estimated_shape(landmark_index, 1) += delta_y;
				}
			}

			// compute error
			double average_error = 0;	// %
			for(int augmented_data_index = 0;augmented_data_index < _num_augmented_data;augmented_data_index++){
				double error = 0;

				for(int landmark_index = 0;landmark_index < _model->_num_landmarks;landmark_index++){
					cv::Mat1d &target_shape = _augmented_target_shapes[augmented_data_index];
					cv::Mat1d &estimated_shape = _augmented_estimated_shapes[augmented_data_index];

					assert(target_shape.rows == _model->_num_landmarks && target_shape.cols == 2);
					assert(estimated_shape.rows == _model->_num_landmarks && estimated_shape.cols == 2);

					double error_x = target_shape(landmark_index, 0) - estimated_shape(landmark_index, 0);
					double error_y = target_shape(landmark_index, 1) - estimated_shape(landmark_index, 1);
					error += std::sqrt(error_x * error_x + error_y * error_y);
				}
				int data_index = _augmented_indices_to_data_index[augmented_data_index];
				double pupil_distance = _training_corpus->get_normalized_pupil_distance(data_index);
				assert(pupil_distance > 0);
				average_error += error / _model->_num_landmarks / pupil_distance * 100;
			}

			average_error /= _num_augmented_data;
			cout << "mean error: " << average_error << " %" << endl;

			for(int augmented_data_index = 0;augmented_data_index < _num_augmented_data;augmented_data_index++){
				delete[] binary_features[augmented_data_index];
			}
			delete[] binary_features;
		}
		void Trainer::train_global_linear_regression_at_stage(int stage, struct liblinear::feature_node** binary_features){
			int num_total_trees = 0;
			int num_total_leaves = 0;
			for(int landmark_index = 0;landmark_index < _model->_num_landmarks;landmark_index++){
				Forest* forest = _model->get_forest(stage, landmark_index);
				num_total_trees += forest->get_num_trees();
				num_total_leaves += forest->get_num_total_leaves();
			}
			cout << "#trees = " << num_total_trees << endl;
			cout << "#features = " << num_total_leaves << endl;

			struct liblinear::problem* problem = new struct liblinear::problem;
			problem->l = _num_augmented_data;
			problem->n = num_total_leaves;
			problem->x = binary_features;
			problem->bias = -1;

			struct liblinear::parameter* parameter = new struct liblinear::parameter;
			parameter->solver_type = liblinear::L2R_L2LOSS_SVR_DUAL;
			parameter->C = 0.00001;
			parameter->p = 0;

		    double** targets = new double*[_model->_num_landmarks];
			for(int landmark_index = 0;landmark_index < _model->_num_landmarks;landmark_index++){
				targets[landmark_index] = new double[_num_augmented_data];
			}

			// train regressor
			cout << "training global linear regressors ..." << endl;
			#pragma omp parallel for
			for(int landmark_index = 0;landmark_index < _model->_num_landmarks;landmark_index++){
				// train x
				for(int augmented_data_index = 0;augmented_data_index < _num_augmented_data;augmented_data_index++){
					cv::Mat1d &target_shape = _augmented_target_shapes[augmented_data_index];
					cv::Mat1d &estimated_shape = _augmented_estimated_shapes[augmented_data_index];

					assert(target_shape.rows == _model->_num_landmarks && target_shape.cols == 2);
					assert(estimated_shape.rows == _model->_num_landmarks && estimated_shape.cols == 2);

					double delta_x = target_shape(landmark_index, 0) - estimated_shape(landmark_index, 0);	// normalized delta

					targets[landmark_index][augmented_data_index] = delta_x;
				}
				problem->y = targets[landmark_index];
				liblinear::check_parameter(problem, parameter);
		        struct liblinear::model* model_x = liblinear::train(problem, parameter);

				// train y
				for(int augmented_data_index = 0;augmented_data_index < _num_augmented_data;augmented_data_index++){
					cv::Mat1d &target_shape = _augmented_target_shapes[augmented_data_index];
					cv::Mat1d &estimated_shape = _augmented_estimated_shapes[augmented_data_index];

					assert(target_shape.rows == _model->_num_landmarks && target_shape.cols == 2);
					assert(estimated_shape.rows == _model->_num_landmarks && estimated_shape.cols == 2);

					double delta_y = target_shape(landmark_index, 1) - estimated_shape(landmark_index, 1);	// normalized delta

					targets[landmark_index][augmented_data_index] = delta_y;
				}
				problem->y = targets[landmark_index];
				liblinear::check_parameter(problem, parameter);
		        struct liblinear::model* model_y = liblinear::train(problem, parameter);

		        _model->set_linear_models(model_x, model_y, stage, landmark_index);
				cout << "." << flush;
			}

			cout << endl;

			for(int landmark_index = 0;landmark_index < _model->_num_landmarks;landmark_index++){
				delete[] targets[landmark_index];
			}
			delete[] targets;
			delete problem;
			delete parameter;
		}
		void Trainer::train_local_feature_mapping_functions(int stage){
			cout << "training local feature mapping functions ..." << endl;
			#pragma omp parallel for
			for(int landmark_index = 0;landmark_index < _model->_num_landmarks;landmark_index++){
				_train_forest(stage, landmark_index);
				cout << "." << flush;
			}
			cout << endl;
		}
		void Trainer::_train_forest(int stage, int landmark_index){
			Corpus* corpus = _training_corpus;
			Forest* forest = _model->get_forest(stage, landmark_index);

			std::vector<FeatureLocation> sampled_feature_locations = _sampled_feature_locations_at_stage[stage];
			assert(sampled_feature_locations.size() == _num_features_to_sample);

			int num_data = corpus->_images.size();
			int augmentation_size = _augmentation_size;

			// pixel differece features
			cv::Mat_<int> pixel_differences(_num_features_to_sample, _num_augmented_data);

			// get pixel differences
			for(int augmented_data_index = 0;augmented_data_index < _num_augmented_data;augmented_data_index++){
				cv::Mat1b &image = get_image_by_augmented_index(augmented_data_index);
				cv::Mat1d projected_shape = project_current_estimated_shape(augmented_data_index);
				_compute_pixel_differences(projected_shape, image, pixel_differences, sampled_feature_locations, augmented_data_index, landmark_index);
			}

			// compute ground truth shape increment	
			std::vector<cv::Mat1d> regression_targets_of_data(_num_augmented_data);	
			for(int augmented_data_index = 0;augmented_data_index < _num_augmented_data;augmented_data_index++){
				cv::Mat1d &target_shape = _augmented_target_shapes[augmented_data_index];
				cv::Mat1d &estimated_shape = _augmented_estimated_shapes[augmented_data_index];

				assert(target_shape.rows == _model->_num_landmarks && target_shape.cols == 2);
				assert(estimated_shape.rows == _model->_num_landmarks && estimated_shape.cols == 2);

				cv::Mat1d regression_targets = target_shape - estimated_shape;

				regression_targets_of_data[augmented_data_index] = regression_targets;
			}
			forest->train(sampled_feature_locations, pixel_differences, regression_targets_of_data);
		}
		void Trainer::_compute_pixel_differences(cv::Mat1d &shape, 
												 cv::Mat1b &image, 
												 cv::Mat_<int> &pixel_differences, 
												 std::vector<FeatureLocation> &sampled_feature_locations,
												 int data_index, 
												 int landmark_index)
		{
			assert(shape.rows == _model->_num_landmarks && shape.cols == 2);
			assert(pixel_differences.rows == _num_features_to_sample && pixel_differences.cols == _num_augmented_data);
			assert(sampled_feature_locations.size() == _num_features_to_sample);

			int image_height = image.rows;
			int image_width = image.cols;

			double landmark_x = shape(landmark_index, 0);	// [-1, 1] : origin is the center of the image
			double landmark_y = shape(landmark_index, 1);	// [-1, 1] : origin is the center of the image

			for(int feature_index = 0;feature_index < _num_features_to_sample;feature_index++){
				FeatureLocation &local_location = sampled_feature_locations[feature_index]; // origin is the landmark position

				// a
				double local_x_a = local_location.a.x + landmark_x;	// [-1, 1] : origin is the center of the image
				double local_y_a = local_location.a.y + landmark_y;
				int pixel_x_a = (image_width / 2.0) + local_x_a * (image_width / 2.0);	// [0, image_width]
				int pixel_y_a = (image_height / 2.0) + local_y_a * (image_height / 2.0);

				// b
				double local_x_b = local_location.b.x + landmark_x;
				double local_y_b = local_location.b.y + landmark_y;
				int pixel_x_b = (image_width / 2.0) + local_x_b * (image_width / 2.0);
				int pixel_y_b = (image_height / 2.0) + local_y_b * (image_height / 2.0);

				// clip bounds
				pixel_x_a = std::max(0, std::min(pixel_x_a, image_width - 1));
				pixel_y_a = std::max(0, std::min(pixel_y_a, image_height - 1));
				pixel_x_b = std::max(0, std::min(pixel_x_b, image_width - 1));
				pixel_y_b = std::max(0, std::min(pixel_y_b, image_height - 1));

				// get pixel value
				int luminosity_a = image(pixel_y_a, pixel_x_a);
				int luminosity_b = image(pixel_y_b, pixel_x_b);

				// pixel difference feature
				int diff = luminosity_a - luminosity_b;

				pixel_differences(feature_index, data_index) = diff;
			}
		}
		cv::Mat1d Trainer::project_current_estimated_shape(int augmented_data_index){
			assert(augmented_data_index < _augmented_estimated_shapes.size());
			cv::Mat1d shape = _augmented_estimated_shapes[augmented_data_index].clone();	// make a copy

			assert(shape.rows == _model->_num_landmarks && shape.cols == 2);

			Corpus* corpus = _training_corpus;
			int data_index = get_data_index_by_augmented_index(augmented_data_index);

			cv::Mat1d &rotation_inv = corpus->get_rotation_inv(data_index);
			cv::Point2d &shift_inv_point = corpus->get_shift_inv(data_index);
			return utils::project_shape(shape, rotation_inv, shift_inv_point);
		}
		np::ndarray Trainer::python_get_target_shape(int augmented_data_index, bool transform){
			assert(augmented_data_index < _augmented_target_shapes.size());
			cv::Mat1d shape = _augmented_target_shapes[augmented_data_index];

			assert(shape.rows == _model->_num_landmarks && shape.cols == 2);

			if(transform){
				Corpus* corpus = _training_corpus;
				int data_index = get_data_index_by_augmented_index(augmented_data_index);

				cv::Mat1d &rotation_inv = corpus->get_rotation_inv(data_index);
				cv::Point2d &shift_inv_point = corpus->get_shift_inv(data_index);
				shape = utils::project_shape(shape, rotation_inv, shift_inv_point);
			}
			
			return utils::cv_matrix_to_ndarray_matrix(shape);
		}
		np::ndarray Trainer::python_get_current_estimated_shape(int augmented_data_index, bool transform){
			assert(augmented_data_index < _augmented_estimated_shapes.size());
			if(transform){
				cv::Mat1d shape = project_current_estimated_shape(augmented_data_index);
				return utils::cv_matrix_to_ndarray_matrix(shape);
			}
			cv::Mat1d shape = _augmented_estimated_shapes[augmented_data_index];
			return utils::cv_matrix_to_ndarray_matrix(shape);
		}
		np::ndarray Trainer::python_estimate_shape_only_using_local_binary_features(int stage, int augmented_data_index, bool transform){
			assert(augmented_data_index < _augmented_estimated_shapes.size());
			cv::Mat1d shape = _augmented_estimated_shapes[augmented_data_index].clone();

			assert(shape.rows == _model->_num_landmarks && shape.cols == 2);

			cv::Mat1d projected_shape = project_current_estimated_shape(augmented_data_index);
			cv::Mat1b &image = get_image_by_augmented_index(augmented_data_index);

			assert(projected_shape.rows == _model->_num_landmarks && projected_shape.cols == 2);

			for(int landmark_index = 0;landmark_index < _model->_num_landmarks;landmark_index++){
				// find leaves
				Forest* forest = _model->get_forest(stage, landmark_index);
				std::vector<Node*> leaves;
				forest->predict(projected_shape, image, leaves);
				assert(leaves.size() == forest->get_num_trees());
				cv::Point2d mean_delta;
				mean_delta.x = 0;
				mean_delta.y = 0;
				// delta_shape
				for(int tree_index = 0;tree_index < forest->get_num_trees();tree_index++){
					Node* leaf = leaves[tree_index];
					mean_delta.x += leaf->_delta_shape.x;
					mean_delta.y += leaf->_delta_shape.y;
				}
				mean_delta.x /= forest->get_num_trees();
				mean_delta.y /= forest->get_num_trees();

				shape(landmark_index, 0) += mean_delta.x;
				shape(landmark_index, 1) += mean_delta.y;
			}

			if(transform){
				Corpus* corpus = _training_corpus;
				int data_index = get_data_index_by_augmented_index(augmented_data_index);

				cv::Mat1d &rotation_inv = corpus->get_rotation_inv(data_index);
				cv::Point2d &shift_inv_point = corpus->get_shift_inv(data_index);
				shape = utils::project_shape(shape, rotation_inv, shift_inv_point);
			}
			return utils::cv_matrix_to_ndarray_matrix(shape);
		}
		boost::python::numpy::ndarray Trainer::python_get_validation_estimated_shape(int data_index, bool transform){
			assert(data_index < _validation_corpus->get_num_images());

			cv::Mat1b &image = _validation_corpus->get_image(data_index);
			cv::Mat1d estimated_shape = _model->_mean_shape.clone();
			
			assert(estimated_shape.rows == _model->_num_landmarks && estimated_shape.cols == 2);

			cv::Mat1d &rotation_inv = _validation_corpus->get_rotation_inv(data_index);
			cv::Point2d &shift_inv_point = _validation_corpus->get_shift_inv(data_index);

			for(int stage = 0;stage < _model->_num_stages;stage++){
				if(_model->_training_finished_at_stage[stage] == false){
					continue;
				}

				// unnormalize initial shape
				cv::Mat1d unnormalized_estimated_shape = utils::project_shape(estimated_shape, rotation_inv, shift_inv_point);

				// compute binary features
				struct liblinear::feature_node* binary_features = _model->compute_binary_features_at_stage(image, unnormalized_estimated_shape, stage);

				for(int landmark_index = 0;landmark_index < _model->_num_landmarks;landmark_index++){
					struct liblinear::model* model_x = _model->get_linear_model_x_at(stage, landmark_index);
					struct liblinear::model* model_y = _model->get_linear_model_y_at(stage, landmark_index);
					assert(model_x != NULL);
					assert(model_y != NULL);

					double delta_x = liblinear::predict(model_x, binary_features);
					double delta_y = liblinear::predict(model_y, binary_features);

					// update shape
					estimated_shape(landmark_index, 0) += delta_x;
					estimated_shape(landmark_index, 1) += delta_y;
				}
				delete[] binary_features;
			}

			if(transform){
				estimated_shape = utils::project_shape(estimated_shape, rotation_inv, shift_inv_point);
			}

			return utils::cv_matrix_to_ndarray_matrix(estimated_shape);
		}
		void Trainer::evaluate_stage(int target_stage){
			cout << "validation stage: " << (target_stage + 1) << " of " << _model->_num_stages << endl;

			int num_data = _validation_corpus->get_num_images();
			std::vector<double> average_error_at_stage(target_stage + 1);
			for(int stage = 0;stage <= target_stage;stage++){
				average_error_at_stage[stage] = 0;	
			}

			for(int data_index = 0;data_index < num_data;data_index++){
					cv::Mat1b &image = _validation_corpus->get_image(data_index);
					cv::Mat1d &target_shape = _validation_corpus->get_normalized_shape(data_index);
					cv::Mat1d &rotation_inv = _validation_corpus->get_rotation_inv(data_index);
					cv::Point2d &shift_inv_point = _validation_corpus->get_shift_inv(data_index);
					cv::Mat1d shift_inv = cv::point_to_mat(shift_inv_point);
					double pupil_distance = _validation_corpus->get_normalized_pupil_distance(data_index);
					std::vector<double> error_at_stage = _model->compute_error(image, target_shape, rotation_inv, shift_inv, pupil_distance);
					assert(error_at_stage.size() >= target_stage);
					for(int stage = 0;stage <= target_stage;stage++){
						average_error_at_stage[stage] += error_at_stage[stage];
					}
			}
			for(int stage = 0;stage <= target_stage;stage++){
				average_error_at_stage[stage] /= num_data;	
			}

			std::cout << "validation error: " << std::endl;
			for(int stage = 0;stage < average_error_at_stage.size();stage++){
				std::cout << "	stage " << stage << ": " << average_error_at_stage[stage] << " %" << std::endl;
			}
		}
	}
}