import argparse, os, sys
import cv2
import numpy as np

class BoundingBox:
	def __init__(self):
		self.left = 0
		self.top = 0
		self.right = 0
		self.bottom = 0

	def move_y(self, move):
		self.top += move
		self.bottom += move

	def move_x(self, move):
		self.top += move
		self.bottom += move

	def cast(self):
		self.left = int(self.left)
		self.top = int(self.top)
		self.right = int(self.right)
		self.bottom = int(self.bottom)

	def width(self):
		return self.right - self.left

	def height(self):
		return self.bottom - self.top

def load_annotations(directory):
	annotations = {}
	fs = os.listdir(directory)
	for filename in fs:
		if filename.endswith(".pts"):
			with open(os.path.join(directory, filename), "r") as f:
				annotation = f.read().strip().split("\n")
				assert len(annotation) == 72
				landmarks = []
				for location_str in annotation[3:-1]:
					location = location_str.split(" ")
					landmarks.append((float(location[0]), float(location[1])))
				annotations[filename.replace(".pts", "")] = landmarks
	return annotations

# expand the bounding box
def expand_bounding_box(bbox, image_height, image_width, padding):
	padding = min(padding, bbox.left)
	padding = min(padding, bbox.top)
	padding = min(padding, image_width - bbox.right)
	padding = min(padding, image_height - bbox.bottom)

	bbox.left = bbox.left - padding
	bbox.top = bbox.top - padding
	bbox.right = bbox.right + padding
	bbox.bottom = bbox.bottom + padding

	bbox.cast()

def get_bounding_box(landmarks, image_height, image_width):
	bbox = BoundingBox()
	bbox.left = landmarks[0][0]
	bbox.top = landmarks[0][1]
	bbox.right = landmarks[0][0]
	bbox.bottom = landmarks[0][1]

	for (x, y) in landmarks:
		if x > bbox.right:
			bbox.right = x
		if x < bbox.left:
			bbox.left = x
		if y > bbox.bottom:
			bbox.bottom = y
		if y < bbox.top:
			bbox.top = y

	bbox.cast()

	# make the bounding box square
	bbox_width = bbox.width()
	bbox_height = bbox.height()

	if bbox_width > bbox_height:
		diff = (bbox_width - bbox_height) // 2
		mod = (bbox_width - bbox_height) % 2
		bbox.top = bbox.top - diff
		bbox.bottom = bbox.bottom + diff + mod
		if bbox.top < 0:
			bbox.move_y(-bbox.top)
			assert bbox.bottom <= image_height
		elif bbox.bottom > image_height:
			bbox.move_y(image_height - bbox.bottom)
			assert bbox.top >= 0

	elif bbox_width < bbox_height:
		diff = (bbox_height - bbox_width) // 2
		mod = (bbox_height - bbox_width) % 2
		bbox.left = bbox.left - diff
		bbox.right = bbox.right + diff + mod
		if bbox.left < 0:
			bbox.move_x(-bbox.left)
			assert bbox.right <= image_width
		elif bbox.right > image_width:
			bbox.move_x(image_width - bbox.right)
			assert bbox.left >= 0

	assert bbox.width() == bbox.height()
	return bbox

def preprocess_images(directory):
	print("processing", directory)
	annotations = load_annotations(directory)
	fs = os.listdir(directory)
	num_total_images = 0
	dataset_images = []
	dataset_landmarks = []
	mean_normalized_landmarks = []
	for _ in range(68):
		mean_normalized_landmarks.append([0, 0])

	for filename in fs:
		if filename.endswith(".png") or filename.endswith("jpg"):
			image_rgb = cv2.imread(os.path.join(directory, filename))
			image_gray = cv2.cvtColor(image_rgb, cv2.COLOR_BGR2GRAY)

			filename = filename.replace(".png", "")
			filename = filename.replace(".jpg", "")
			assert filename in annotations
			landmarks = annotations[filename]

			image_height = image_rgb.shape[0]
			image_width = image_rgb.shape[1]

			try:
				bbox = get_bounding_box(landmarks, image_height, image_width)
				padding = bbox.width() * 0.3
				expand_bounding_box(bbox, image_height, image_width, padding)
			except Exception as e:
				continue

			image_gray = image_gray[bbox.top:bbox.bottom + 1, bbox.left:bbox.right + 1]
			if bbox.width() > args.max_image_size:
				image_gray = cv2.resize(image_gray, (args.max_image_size, args.max_image_size))

			dataset_images.append(image_gray)

			# normalize landmark location
			# x: [-1, 1]
			# y: [-1, 1]
			normalized_landmarks = []
			for feature_index, (x, y) in enumerate(landmarks):
				x = (x - bbox.left) / bbox.width() * 2 - 1
				y = (y - bbox.top) / bbox.height() * 2 - 1
				normalized_landmarks.append((x, y))

			dataset_landmarks.append(normalized_landmarks)

	return dataset_images, dataset_landmarks

def build_corpus():
	image_list_train = []
	shape_list_train = []
	targets = ["01_Indoor", "02_Outdoor"]
	targets = ["00_Test"]

	mean_shape = []
	for _ in range(68):
		mean_shape.append([0, 0])

	for target in targets:
		images, shape = preprocess_images(os.path.join(args.dataset_directory, target))
		image_list_train += images
		shape_list_train += shape

	# calculate mean shape
	for shape in shape_list_train:
		for feature_index, (x, y) in enumerate(shape):
			mean_shape[feature_index][0] += x
			mean_shape[feature_index][1] += y
		
	for feature_index in range(len(mean_shape)):
		mean_shape[feature_index][0] /= len(shape_list_train)
		mean_shape[feature_index][1] /= len(shape_list_train)

	return image_list_train, shape_list_train, mean_shape

def plot_shape(shape, name):
	image = np.zeros((500, 500), dtype=np.uint8)
	white = (255, 255, 255)
	for (x, y) in shape:
		x = int(250 + x * 250)
		y = int(250 + y * 250)
		cv2.line(image, (x - 4, y), (x + 4, y), white, 1)
		cv2.line(image, (x, y - 4), (x, y + 4), white, 1)
	cv2.imwrite(os.path.join(args.output_directory, "{}.png".format(name)), image)

def main():
	assert args.dataset_directory is not None
	assert args.output_directory is not None

	try:
		os.mkdir(args.output_directory)
	except:
		pass

	# build corpus
	image_list_train, shape_list_train, mean_shape = build_corpus()
	mean_shape = np.asarray(mean_shape, dtype=np.float64)

	# save mean shape
	plot_shape(mean_shape, "mean")

	# rigid transform
	for index, (image, shape) in enumerate(zip(image_list_train, shape_list_train)):
		shape = np.asarray(shape, dtype=np.float64)

		mat = cv2.estimateRigidTransform(shape, mean_shape, False)
		if mat is None:
			print("Error estimating rigid transform of", index)
			continue
		rotation = mat[:, :2]
		shift = mat[:, 2]

		normalized_shape = np.transpose(np.dot(rotation, shape.T) + shift[:, None], (1, 0))

		try:
			rotation_inv = np.linalg.inv(rotation)
		except Exception as e:
			continue
		shape_inv = np.transpose(np.dot(rotation_inv, normalized_shape.T - shift[:, None]), (1, 0))

		# save normalized shape
		plot_shape(shape, "{}_original".format(index))
		plot_shape(normalized_shape, "{}_normalized".format(index))
		plot_shape(shape_inv, "{}_inverse".format(index))

	print("#images", len(image_list_train))

if __name__ == "__main__":
	parser = argparse.ArgumentParser()
	parser.add_argument("--dataset-directory", "-dataset", type=str, default=None)
	parser.add_argument("--output-directory", "-out", type=str, default=None)
	parser.add_argument("--max-image-size", "-size", type=int, default=500)
	args = parser.parse_args()
	main()