#ifdef USE_OPENCV
#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <string>
#include <utility>
#include <vector>
#include <algorithm>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/highgui/highgui_c.h>
#include <opencv2/imgproc/imgproc.hpp>

#include "caffe/data_layers.hpp"
#include "caffe/layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

namespace caffe {

template <typename Dtype>
WindowClsDataLayer<Dtype>::~WindowClsDataLayer<Dtype>() {
  this->JoinPrefetchThread();
}

template <typename Dtype>
void WindowClsDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const int new_height = this->layer_param_.image_data_param().new_height();
  const int new_width  = this->layer_param_.image_data_param().new_width();
  const bool is_color  = this->layer_param_.image_data_param().is_color();
  const int label_type = this->layer_param_.image_data_param().label_type();
  string root_folder = this->layer_param_.image_data_param().root_folder();

  TransformationParameter transform_param = this->layer_param_.transform_param();
  CHECK(transform_param.has_mean_file() == false) << 
         "WindowClsDataLayer does not support mean file";
  CHECK((new_height == 0 && new_width == 0) ||
      (new_height > 0 && new_width > 0)) << "Current implementation requires "
      "new_height and new_width to be set at the same time.";

  label_dim_ = this->layer_param_.window_cls_data_param().label_dim();

  // Read the file with filenames and labels
  const string& source = this->layer_param_.image_data_param().source();
  LOG(INFO) << "Opening file " << source;
  std::ifstream infile(source.c_str());

  string linestr;
  while (std::getline(infile, linestr)) {
    std::istringstream iss(linestr);
    string imgfn;
    iss >> imgfn;
    string segfn = "";
    if (label_type != ImageDataParameter_LabelType_NONE) {
      iss >> segfn;
    }
    SEGITEMS item;
    item.imgfn = imgfn;
    item.segfn = segfn;

    int x1, y1, x2, y2;
    iss >> x1 >> y1 >> x2 >> y2;
    item.x1 = x1;
    item.y1 = y1;
    item.x2 = x2;
    item.y2 = y2;

    lines_.push_back(item);
  }

  if (this->layer_param_.image_data_param().shuffle()) {
    // randomly shuffle data
    LOG(INFO) << "Shuffling data";
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
    ShuffleImages();
  }
  LOG(INFO) << "A total of " << lines_.size() << " images.";

  lines_id_ = 0;
  // Check if we would need to randomly skip a few data points
  if (this->layer_param_.image_data_param().rand_skip()) {
    unsigned int skip = caffe_rng_rand() %
        this->layer_param_.image_data_param().rand_skip();
    LOG(INFO) << "Skipping first " << skip << " data points.";
    CHECK_GT(lines_.size(), skip) << "Not enough points to skip";
    lines_id_ = skip;
  }

  // Read an image, and use it to initialize the top blob.
  cv::Mat cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].imgfn,
                                    new_height, new_width, is_color);
  const int channels = cv_img.channels();
  const int height = cv_img.rows;
  const int width = cv_img.cols;
  // image
  const int crop_size = this->layer_param_.transform_param().crop_size();
  const int batch_size = this->layer_param_.image_data_param().batch_size();
  if (crop_size > 0) {
    top[0]->Reshape(batch_size, channels, crop_size, crop_size);
    this->prefetch_data_.Reshape(batch_size, channels, crop_size, crop_size);
    this->transformed_data_.Reshape(1, channels, crop_size, crop_size);

    // transformed label
    this->seg_label_buffer_.Reshape(batch_size, 1, crop_size, crop_size);
    this->transformed_label_.Reshape(1, 1, crop_size, crop_size);  
   
  } else {
    top[0]->Reshape(batch_size, channels, height, width);
    this->prefetch_data_.Reshape(batch_size, channels, height, width);
    this->transformed_data_.Reshape(1, channels, height, width);
    
    // transformed label
    this->seg_label_buffer_.Reshape(batch_size, 1, height, width);
    this->transformed_label_.Reshape(1, 1, height, width);     
  }
  // label
  top[1]->Reshape(batch_size, label_dim_, 1, 1);
  this->prefetch_label_.Reshape(batch_size, label_dim_, 1, 1);
  this->computed_label_.Reshape(1, label_dim_, 1, 1);

  // image dimensions, for each image, stores (img_height, img_width)
  top[2]->Reshape(batch_size, 1, 1, 2);
  this->prefetch_data_dim_.Reshape(batch_size, 1, 1, 2);

  LOG(INFO) << "output data size: " << top[0]->num() << ","
	    << top[0]->channels() << "," << top[0]->height() << ","
	    << top[0]->width();
  // label
  LOG(INFO) << "output label size: " << top[1]->num() << ","
	    << top[1]->channels() << "," << top[1]->height() << ","
	    << top[1]->width();
  // image_dim
  LOG(INFO) << "output data_dim size: " << top[2]->num() << ","
	    << top[2]->channels() << "," << top[2]->height() << ","
	    << top[2]->width();
}

template <typename Dtype>
void WindowClsDataLayer<Dtype>::ShuffleImages() {
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  shuffle(lines_.begin(), lines_.end(), prefetch_rng);
}

// This function is used to create a thread that prefetches the data.
template <typename Dtype>
void WindowClsDataLayer<Dtype>::InternalThreadEntry() {
  CPUTimer batch_timer;
  batch_timer.Start();
  double read_time = 0;
  double trans_time = 0;
  CPUTimer timer;
  CHECK(this->prefetch_data_.count());
  CHECK(this->transformed_data_.count());

  Dtype* top_data     = this->prefetch_data_.mutable_cpu_data();
  Dtype* top_label    = this->prefetch_label_.mutable_cpu_data(); 
  Dtype* top_data_dim = this->prefetch_data_dim_.mutable_cpu_data();
  Dtype* seg_label    = this->seg_label_buffer_.mutable_cpu_data();

  const int max_height = this->prefetch_data_.height();
  const int max_width  = this->prefetch_data_.width();

  ImageDataParameter image_data_param    = this->layer_param_.image_data_param();
  const int batch_size = image_data_param.batch_size();
  const int new_height = image_data_param.new_height();
  const int new_width  = image_data_param.new_width();
  const int label_type = this->layer_param_.image_data_param().label_type();
  const int ignore_label = image_data_param.ignore_label();
  const bool is_color  = image_data_param.is_color();
  string root_folder   = image_data_param.root_folder();

  const int lines_size = lines_.size();
  int top_data_dim_offset;

  for (int item_id = 0; item_id < batch_size; ++item_id) {
    top_data_dim_offset = this->prefetch_data_dim_.offset(item_id);

    std::vector<cv::Mat> cv_img_seg;
    cv::Mat cv_img, cv_seg;

    // get a blob
    timer.Start();
    CHECK_GT(lines_size, lines_id_);

    int img_row, img_col;
    cv_img = ReadImageToCVMat(root_folder + lines_[lines_id_].imgfn,
	  0, 0, is_color, &img_row, &img_col);

    top_data_dim[top_data_dim_offset]     = static_cast<Dtype>(std::min(max_height, img_row));
    top_data_dim[top_data_dim_offset + 1] = static_cast<Dtype>(std::min(max_width, img_col));

    if (!cv_img.data) {
      DLOG(INFO) << "Fail to load img: " << root_folder + lines_[lines_id_].imgfn;
    }
    if (label_type == ImageDataParameter_LabelType_PIXEL) {
      cv_seg = ReadImageToCVMatNearest(root_folder + lines_[lines_id_].segfn,
					    0, 0, false);
      if (!cv_seg.data) {
	DLOG(INFO) << "Fail to load seg: " << root_folder + lines_[lines_id_].segfn;
      }
    }
    else if (label_type == ImageDataParameter_LabelType_IMAGE) {
      const int label = atoi(lines_[lines_id_].segfn.c_str());
      cv::Mat seg(cv_img.rows, cv_img.cols, 
		  CV_8UC1, cv::Scalar(label));
      cv_seg = seg;      
    }
    else {
      cv::Mat seg(cv_img.rows, cv_img.cols, 
		  CV_8UC1, cv::Scalar(ignore_label));
      cv_seg = seg;
    }
    // crop window out of image and warp it
    int x1 = lines_[lines_id_].x1;
    int y1 = lines_[lines_id_].y1;
    int x2 = lines_[lines_id_].x2;
    int y2 = lines_[lines_id_].y2;
    // compute padding 
    int pad_x1 = std::max(0, -x1);
    int pad_y1 = std::max(0, -y1);
    int pad_x2 = std::max(0, x2 - cv_img.cols + 1);
    int pad_y2 = std::max(0, y2 - cv_img.rows + 1);
    if (pad_x1 > 0 || pad_x2 > 0 || pad_y1 > 0 || pad_y2 > 0) {
        cv::copyMakeBorder(cv_img, cv_img, pad_y1, pad_y2,
            pad_x1, pad_x2, cv::BORDER_CONSTANT,
            cv::Scalar(0,0,0)); 
        cv::copyMakeBorder(cv_seg, cv_seg, pad_y1, pad_y2,
            pad_x1, pad_x2, cv::BORDER_CONSTANT,
            cv::Scalar(ignore_label));
    }
    // clip bounds
    x1 = x1 + pad_x1;
    x2 = x2 + pad_x1;
    y1 = y1 + pad_y1;
    y2 = y2 + pad_y1;
    CHECK_GT(x1, -1);
    CHECK_GT(y1, -1);
    CHECK_LT(x2, cv_img.cols);
    CHECK_LT(y2, cv_img.rows);
   
    // cropping
    cv::Rect roi(x1, y1, x2-x1+1, y2-y1+1);
    cv::Mat cv_cropped_img = cv_img(roi);
    cv::Mat cv_cropped_seg = cv_seg(roi);
    if (new_width > 0 && new_height > 0) {
        cv::resize(cv_cropped_img, cv_cropped_img, 
               cv::Size(new_width, new_height), 0, 0, cv::INTER_LINEAR);
        cv::resize(cv_cropped_seg, cv_cropped_seg, 
               cv::Size(new_width, new_height), 0, 0, cv::INTER_NEAREST);
    }
    cv_img_seg.push_back(cv_cropped_img);
    cv_img_seg.push_back(cv_cropped_seg);

    read_time += timer.MicroSeconds();
    timer.Start();
    // Apply transformations (mirror, crop...) to the image
    int offset;

    offset = this->prefetch_data_.offset(item_id);
    this->transformed_data_.set_cpu_data(top_data + offset);

    offset = this->seg_label_buffer_.offset(item_id);
    this->transformed_label_.set_cpu_data(seg_label + offset);

    this->data_transformer_.TransformImgAndSeg(cv_img_seg, 
	 &(this->transformed_data_), &(this->transformed_label_),
	 ignore_label);
    trans_time += timer.MicroSeconds();

    // compute label
    offset = this->prefetch_label_.offset(item_id);
    this->computed_label_.set_cpu_data(top_label + offset);

    const Dtype * one_seg_data = this->transformed_label_.cpu_data();
    Dtype * one_label_data = this->computed_label_.mutable_cpu_data();
    int pixel_cnt = this->transformed_label_.count();
    caffe_set(label_dim_, Dtype(0), one_label_data);
    for (int i = 0; i < pixel_cnt; i++) {
      int pixel_label = one_seg_data[i];
      if (pixel_label != 0 && pixel_label != 255) {
        CHECK_LT(pixel_label-1, label_dim_);
        one_label_data[pixel_label-1] = 1;
      }
    }

    // go to the next std::vector<int>::iterator iter;
    lines_id_++;
    if (lines_id_ >= lines_size) {
      // We have reached the end. Restart from the first.
      DLOG(INFO) << "Restarting data prefetching from start.";
      lines_id_ = 0;
      if (this->layer_param_.image_data_param().shuffle()) {
	ShuffleImages();
      }
    }
  }
  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
  DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
  DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

INSTANTIATE_CLASS(WindowClsDataLayer);
REGISTER_LAYER_CLASS(WINDOW_CLS_DATA, WindowClsDataLayer);
}  // namespace caffe
#endif  // USE_OPENCV