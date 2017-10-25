#include "corpus.h"

using std::cout;
using std::endl;
namespace np = boost::python::numpy;

namespace lbf {
	namespace python {
		void Corpus::add(np::ndarray image_ndarray, 
						 np::ndarray shape_ndarray, 
						 boost::python::numpy::ndarray normalized_shape_ndarray,
						 boost::python::numpy::ndarray rotation,
						 boost::python::numpy::ndarray rotation_inv,
						 boost::python::numpy::ndarray shift)
		{
			_add_ndarray_matrix_to(image_ndarray, _images);
			_add_ndarray_matrix_to(shape_ndarray, _shapes);
			_add_ndarray_matrix_to(normalized_shape_ndarray, _normalized_shapes);
			_add_ndarray_matrix_to(rotation, _rotation);
			_add_ndarray_matrix_to(rotation_inv, _rotation_inv);
			_add_ndarray_point_to(shift, _shift);

			assert(_images.size() == _shift.size());
			assert(_images.size() == _normalized_shapes.size());
			assert(_images.size() == _rotation.size());
			assert(_images.size() == _rotation_inv.size());
		}
		template <typename T>
		void Corpus::_add_ndarray_matrix_to(boost::python::numpy::ndarray &array, std::vector<cv::Mat_<T>> &corpus){
			auto size = array.get_shape();
			auto stride = array.get_strides();
			cv::Mat_<T> mat(size[0], size[1]);
			for (int h = 0; h < size[0]; ++h) {
				for (int w = 0; w < size[1]; ++w) {
					T value = *reinterpret_cast<T*>(array.get_data() + h * stride[0] + w * stride[1]);
					mat(h, w) = value;
				}
			}
			corpus.push_back(mat);
		}
		void Corpus::_add_ndarray_point_to(boost::python::numpy::ndarray &array, std::vector<cv::Point2d> &corpus){
			auto size = array.get_shape();
			auto stride = array.get_strides();
			cv::Point2d point;
			point.x = *reinterpret_cast<double*>(array.get_data());
			point.y = *reinterpret_cast<double*>(array.get_data() + stride[0]);
			corpus.push_back(point);
		}
		// void Corpus::_add_image_to(boost::python::numpy::ndarray &image_ndarray, std::vector<cv::Mat_<uint8_t>> &corpus){
		// 	auto size = image_ndarray.get_shape();
		// 	auto stride = image_ndarray.get_strides();
		// 	cv::Mat_<uint8_t> image(size[0], size[1]);
		// 	for (int h = 0; h < size[0]; ++h) {
		// 		for (int w = 0; w < size[1]; ++w) {
		// 			uint8_t pixel_value = *reinterpret_cast<uint8_t*>(image_ndarray.get_data() + h * stride[0] + w * stride[1]);
		// 			image(h, w) = pixel_value;
		// 		}
		// 	}
		// 	corpus.push_back(image);
		// }
		// void Corpus::_add_shape_to(boost::python::numpy::ndarray &shape_ndarray, std::vector<cv::Mat1d> &corpus){
		// 	auto size = shape_ndarray.get_shape();
		// 	auto stride = shape_ndarray.get_strides();
		// 	cv::Mat1d shape(size[0], size[1]);
		// 	for (int h = 0; h < size[0]; ++h) {
		// 		for (int w = 0; w < size[1]; ++w) {
		// 			double coord = *reinterpret_cast<double*>(shape_ndarray.get_data() + h * stride[0] + w * stride[1]);
		// 			shape(h, w) = coord;
		// 		}
		// 	}
		// 	corpus.push_back(shape);
		// }
		int Corpus::get_num_images(){
			return _images.size();
		}
		cv::Mat1d & Corpus::get_shape(int data_index){
			assert(data_index < _shapes.size());
			return _shapes[data_index];
		}
		cv::Mat1d & Corpus::get_normalized_shape(int data_index){
			assert(data_index < _normalized_shapes.size());
			return _normalized_shapes[data_index];
		}
		cv::Mat_<uint8_t> & Corpus::get_image(int data_index){
			assert(data_index < _images.size());
			return _images[data_index];
		}
		boost::python::numpy::ndarray Corpus::python_get_image(int data_index){
			assert(data_index < _images.size());
			cv::Mat_<uint8_t> &image = _images[data_index];

			boost::python::tuple size = boost::python::make_tuple(image.rows, image.cols);
			np::ndarray image_ndarray = np::zeros(size, np::dtype::get_builtin<uint8_t>());
			for(int h = 0;h < image.rows;h++) {
				for(int w = 0;w < image.cols;w++) {
					image_ndarray[h][w] = image(h, w);
				}
			}
			return image_ndarray;
		}
		cv::Mat1d & Corpus::get_rotation(int data_index){
			assert(data_index < _rotation.size());
		}
		cv::Mat1d & Corpus::get_rotation_inv(int data_index){
			assert(data_index < _rotation_inv.size());
		}
		cv::Point2d & Corpus::get_shift(int data_index){
			assert(data_index < _shift.size());
		}
	}
}