#include <vector>

#include "caffe/filler.hpp"
#include "caffe/layer.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/im2col.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/layers/compress_convolution_layer.hpp"
#include <cmath>
#include <vector>

namespace caffe {

template <typename Dtype>
void CConvolutionLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  BaseConvolutionLayer <Dtype>::LayerSetUp(bottom, top); 
  
  /************ For dynamic network surgery ***************/
  this->name_ = this->layer_param_.name();
  CConvolutionParameter cconv_param = this->layer_param_.cconvolution_param();
  this->blob_num_ = this->blobs_.size();
  this->blobs_.resize(this->blob_num_ + 1);
    // Intialize and fill the weightmask & biasmask
    this->blobs_[this->blob_num_].reset(new Blob<Dtype>(this->blobs_[0]->shape()));
    shared_ptr<Filler<Dtype> > weight_mask_filler(GetFiller<Dtype>(
        cconv_param.weight_mask_filler()));
    weight_mask_filler->Fill(this->blobs_[this->blob_num_].get());

  // Intializing the tmp tensor
  this->weight_tmp_.Reshape(this->blobs_[0]->shape());
	
	// Intialize the hyper-parameters
  
  this->is_pruning_ = cconv_param.is_pruning();
  this->upper_bound_ = cconv_param.upper_bound();
  this->iter_stop_ = cconv_param.iter_stop();
  this->inter_iter_ = cconv_param.inter_iter();
  this->bound_weight_ = this->upper_bound_ / log(
        this->iter_stop_ / (Dtype)this->inter_iter_);
  /********************************************************/
}

template <typename Dtype>
void CConvolutionLayer<Dtype>::compute_output_shape() {
  const int* kernel_shape_data = this->kernel_shape_.cpu_data();
  const int* stride_data = this->stride_.cpu_data();
  const int* pad_data = this->pad_.cpu_data();
  const int* dilation_data = this->dilation_.cpu_data();
  this->output_shape_.clear();
  for (int i = 0; i < this->num_spatial_axes_; ++i) {
    // i + 1 to skip channel axis
    const int input_dim = this->input_shape(i + 1);
    const int kernel_extent = dilation_data[i] * (kernel_shape_data[i] - 1) + 1;
    const int output_dim = (input_dim + 2 * pad_data[i] - kernel_extent)
        / stride_data[i] + 1;
    this->output_shape_.push_back(output_dim);
  }
}

template <typename Dtype>
void CConvolutionLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {  
      
  const Dtype* weight = this->blobs_[0]->mutable_cpu_data();    
  Dtype* weightMask = this->blobs_[this->blob_num_]->mutable_cpu_data(); 
  Dtype* weightTmp = this->weight_tmp_.mutable_cpu_data(); 

  const Dtype* bias = NULL;
  if (this->bias_term_) {
    bias = this->blobs_[1]->mutable_cpu_data(); 
  }

  if (this->phase_ == TRAIN){
		// Calculate the mean and standard deviation of learnable parameters 
    if (this->iter_ % this->inter_iter_ == 0 && (this->iter_) < (this->iter_stop_)){      
        // compute the weight mask based on the inter_inter
        Dtype sparsity_ratio = this->bound_weight_ * log(2 + (this->iter_ / this->inter_iter_));
		// compute the mask
		caffe_set(this->blobs_[this->blob_num_]->count(), (Dtype)1.0, weightMask);
		vector<std::pair <Dtype, size_t> > param_temp;
		for (size_t i = 0; i < this->blobs_[this->blob_num_]->count(); i++)
			param_temp.push_back(std::make_pair(fabs(weightMask[i]), i));

		std::sort(param_temp.begin(), param_temp.end(), sortPairAscend);
		for (size_t i = 0; i < param_temp.size() * sparsity_ratio; i++)
			weightMask[param_temp[i].second] = 0.0;

	}
  } 
  // Calculate the current (masked) weight and bias
  caffe_mul(this->blobs_[0]->count(), weightMask, weight, weightTmp);
  
  // Forward calculation with (masked) weight and bias 
  for (int i = 0; i < bottom.size(); ++i) {
    const Dtype* bottom_data = bottom[i]->cpu_data();
    Dtype* top_data = top[i]->mutable_cpu_data();
    for (int n = 0; n < this->num_; ++n) {
      this->forward_cpu_gemm(bottom_data + bottom[i]->offset(n), weightTmp,
          top_data + top[i]->offset(n));
      if (this->bias_term_) {
        this->forward_cpu_bias(top_data + top[i]->offset(n), bias);
      }
    }
  }
}

template <typename Dtype>
void CConvolutionLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
	const Dtype* weightTmp = this->weight_tmp_.cpu_data();  
	const Dtype* weightMask = this->blobs_[this->blob_num_]->cpu_data();
	Dtype* weight_diff = this->blobs_[0]->mutable_cpu_diff();
  for (int i = 0; i < top.size(); ++i) {
    const Dtype* top_diff = top[i]->cpu_diff();    
    // Bias gradient, if necessary.
    if (this->bias_term_ && this->param_propagate_down_[1]) {
        Dtype* bias_diff = this->blobs_[1]->mutable_cpu_diff();			
      for (int n = 0; n < this->num_; ++n) {
        this->backward_cpu_bias(bias_diff, top_diff + top[i]->offset(n));
      }
    }
    if (this->param_propagate_down_[0] || propagate_down[i]) {
		const Dtype* bottom_data = bottom[i]->cpu_data();
		Dtype* bottom_diff = bottom[i]->mutable_cpu_diff();	
      for (int n = 0; n < this->num_; ++n) {
        // gradient w.r.t. weight. Note that we will accumulate diffs.
        if (this->param_propagate_down_[0]) {
          this->weight_cpu_gemm(bottom_data + bottom[i]->offset(n),
              top_diff + top[i]->offset(n), weight_diff);
        }
        // gradient w.r.t. bottom data, if necessary.
        if (propagate_down[i]) {
          this->backward_cpu_gemm(top_diff + top[i]->offset(n), weightTmp,
              bottom_diff + bottom[i]->offset(n));
        }
      }
	  caffe_mul(this->blobs_[0]->count(), weight_diff, weightMask, weight_diff);
    }
  }
}

#ifdef CPU_ONLY
STUB_GPU(CConvolutionLayer);
#endif

INSTANTIATE_CLASS(CConvolutionLayer);
REGISTER_LAYER_CLASS(CConvolution);

}  // namespace caffe
