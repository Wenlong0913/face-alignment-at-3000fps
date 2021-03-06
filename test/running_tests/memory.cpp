#include <boost/python/numpy.hpp>
#include <opencv2/opencv.hpp>
#include <fstream>
#include <vector>
#include <string>
#include <set>
#include "../../src/lbf/sampler.h"
#include "../../src/python/corpus.h"
#include "../../src/python/dataset.h"
#include "../../src/python/model.h"
#include "../../src/python/trainer.h"

using namespace lbf;
using namespace lbf::randomforest;
using namespace lbf::python;
using std::cout;
using std::endl;
namespace p = boost::python;
namespace np = boost::python::numpy;

void read_shape(std::string filename, cv::Mat1d &shape){
	double* array = new double[68 * 2];
	std::ifstream ifs(filename, std::ios::in | std::ios::binary);
	if(ifs.is_open() == false){
		cout << "Error reading " << filename << endl;
		exit(0);
	}
    ifs.seekg(0, std::ios::beg);
	ifs.read(reinterpret_cast<char*>(array), 68 * 2 * sizeof(double));
	for(int h = 0;h < 68;h++){
		shape(h, 0) = array[h * 2 + 0];
		shape(h, 1) = array[h * 2 + 1];
	}
	ifs.close();
	delete[] array;
}

void read_rotation(std::string filename, cv::Mat1d &rotation){
	double* array = new double[68 * 2];
	std::ifstream ifs(filename, std::ios::in | std::ios::binary);
	if(ifs.is_open() == false){
		cout << "Error reading " << filename << endl;
		exit(0);
	}
    ifs.seekg(0, std::ios::beg);
	ifs.read(reinterpret_cast<char*>(array), 4 * sizeof(double));
	rotation(0, 0) = array[0];
	rotation(0, 1) = array[1];
	rotation(1, 0) = array[2];
	rotation(1, 1) = array[3];
	ifs.close();
	delete[] array;
}

void read_shift(std::string filename, cv::Point2d &shift){
	double* array = new double[68 * 2];
	std::ifstream ifs(filename, std::ios::in | std::ios::binary);
	if(ifs.is_open() == false){
		cout << "Error reading " << filename << endl;
		exit(0);
	}
    ifs.seekg(0, std::ios::beg);
	ifs.read(reinterpret_cast<char*>(array), 4 * sizeof(double));
	shift.x = array[0];
	shift.y = array[1];
	ifs.close();
	delete[] array;
}

Corpus* build_corpus(std::string directory, cv::Mat1d &mean_shape, int num_data){
	for(int h = 0;h < 68;h++){
		mean_shape(h, 0) = 0;
		mean_shape(h, 1) = 0;
	}
	Corpus* corpus = new Corpus();
	for(int data_index = 0;data_index < num_data;data_index++){
		std::string image_filename = directory + std::to_string(data_index) + ".jpg";
		std::string rotation_filename = directory + std::to_string(data_index) + ".rotation";
		std::string rotation_inv_filename = directory + std::to_string(data_index) + ".rotation_inv";
		std::string shift_filename = directory + std::to_string(data_index) + ".shift";
		std::string shift_inv_filename = directory + std::to_string(data_index) + ".shift_inv";

		cv::Mat1b image = cv::imread(image_filename.c_str(), 0);
		if(image.data == NULL){
			continue;
		}
		cv::Mat1d shape(68, 2);
		cv::Mat1d normalized_shape(68, 2);
		cv::Mat1d rotation(2, 2);
		cv::Mat1d rotation_inv(2, 2);
		cv::Point2d shift;
		cv::Point2d shift_inv;
		
		read_shape(directory + std::to_string(data_index) + ".shape", shape);
		read_shape(directory + std::to_string(data_index) + ".nshape", normalized_shape);
		read_rotation(directory + std::to_string(data_index) + ".rotation", rotation);
		read_rotation(directory + std::to_string(data_index) + ".rotation_inv", rotation_inv);
		read_shift(directory + std::to_string(data_index) + ".shift", shift);
		read_shift(directory + std::to_string(data_index) + ".shift_inv", shift_inv);

		corpus->_images.push_back(image);
		corpus->_shapes.push_back(shape);
		corpus->_normalized_shapes.push_back(shape);
		corpus->_rotation.push_back(rotation);
		corpus->_rotation_inv.push_back(rotation_inv);
		corpus->_shift.push_back(shift);
		corpus->_shift_inv.push_back(shift_inv);
		corpus->_normalized_pupil_distances.push_back(1.0);

		mean_shape += shape;
	}
	mean_shape /= corpus->get_num_images();
	return corpus;
}

void run_training_loop(){
	std::string directory = "/media/aibo/e9ef3312-af31-4750-a797-18efac730bc5/sandbox/face-alignment/cpp";
	// std::string directory = "/media/stark/HDD/sandbox/face-alignment/cpp";
	cv::Mat1d mean_shape_train(68, 2);
	cv::Mat1d mean_shape_dev(68, 2);
	Corpus* training_corpus = build_corpus(directory + "/train/", mean_shape_train, 3229);
	Corpus* validation_corpus = build_corpus(directory + "/dev/", mean_shape_dev, 553);

	boost::python::tuple size = boost::python::make_tuple(mean_shape_train.rows, mean_shape_train.cols);
	np::ndarray mean_shape_ndarray = np::zeros(size, np::dtype::get_builtin<double>());
	for(int h = 0;h < mean_shape_train.rows;h++) {
		for(int w = 0;w < mean_shape_train.cols;w++) {
			mean_shape_ndarray[h][w] = mean_shape_train(h, w);
		}
	}

	int augmentation_size = 5;
	int num_stages = 5;
	int num_trees_per_forest = 20;
	int tree_depth = 7;
	int num_landmarks = 68;
	int num_features_to_sample = 500;
	std::vector<double> feature_radius{0.29, 0.21, 0.16, 0.12, 0.08, 0.04};

	cout << "#images " << training_corpus->get_num_images() << endl;
	cout << "#images " << validation_corpus->get_num_images() << endl;

	Model* model = new Model(num_stages, num_trees_per_forest, tree_depth, num_landmarks, mean_shape_ndarray, feature_radius);
	std::string filename = "lbf.model";
	model->python_save(filename);

	Trainer* trainer = new Trainer(training_corpus, validation_corpus, model, augmentation_size, num_features_to_sample);
	trainer->train();
	// for(int stage = 0;stage < 2;stage++){
	// 	trainer->train_local_feature_mapping_functions(stage);
	// }

	delete training_corpus;
	delete validation_corpus;
	delete model;
	delete trainer;
}

int main(){
	Py_Initialize();
	np::initialize();

	for(int i = 0;i < 10;i++){
		run_training_loop();
	}
}