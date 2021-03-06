#include "ATen/ATen.h"
#include "ATen/Error.h"
#include "ATen/ExpandUtils.h"
#include "ATen/NativeFunctions.h"
#include "ATen/WrapDimUtils.h"
#include "ATen/optional.h"
#include <TH/THTensor.hpp>

#include <algorithm>
#include <vector>

namespace at {
namespace native {

static void check_cat_no_zero_dim(TensorList tensors) {
  for(size_t i = 0; i < tensors.size(); ++i) {
    auto& t = tensors[i];
    if (t.dim() == 0) {
      AT_ERROR("zero-dimensional tensor (at position ", i, ") cannot be concatenated");
    }
  }
}

Tensor & cat_out(Tensor & result, TensorList tensors, int64_t dim) {
  check_cat_no_zero_dim(tensors);
  dim = legacy_cat_wrap_dim(dim, tensors);
  return at::_cat_out(result, tensors, dim);
}

Tensor cat(TensorList tensors, int64_t dim) {
  check_cat_no_zero_dim(tensors);
  dim = legacy_cat_wrap_dim(dim, tensors);
  return at::_cat(tensors, dim);
}

std::vector<Tensor> chunk(const Tensor& self, int64_t chunks, int64_t dim) {
  if (self.dim() == 0) {
    AT_ERROR("chunk expects at least a 1-dimensional tensor");
  }
  if (chunks <= 0) {
    AT_ERROR("chunk expects `chunks` to be greater than 0, got: ", chunks);
  }
  int64_t split_size = (self.size(dim) + chunks - 1) / chunks;

  // We need to call split_with_sizes in the case where split_size and dimension size are 0, because
  // a call to split would discard the number of chunks (because we can have an arbitrary number of
  // 0-sized chunks adding up to 0).  So, call split_with_sizes with the correct number of chunks,
  // eventually we will do this for all cases.
  if (split_size == 0 && self.size(dim) == 0) {
    std::vector<int64_t> split_sizes(chunks, split_size);
    split_sizes[chunks - 1] = split_size - (split_size * chunks - self.size(dim));
    return self.split_with_sizes(split_sizes, dim);
  } else {
    return self.split(split_size, dim);
  }
}

Tensor diagflat(const Tensor& self, int64_t offset) {
  return self.contiguous().view(-1).diag(offset);
}

Tensor diagonal(const Tensor& self, int64_t offset, int64_t dim1_, int64_t dim2_) {
  int64_t nDims = self.dim();
  int64_t dim1 = maybe_wrap_dim(dim1_, nDims);
  int64_t dim2 = maybe_wrap_dim(dim2_, nDims);
  AT_CHECK(dim1 != dim2, "diagonal dimensions cannot be identical ", dim1_, ", ", dim2_);
  int64_t diag_size;
  int64_t storage_offset = self.storage_offset();
  // compute storage offset and size for the diagonal
  // for positive values of offset (above the main diagonal)
  // "leftmost columns" (along dim2) are dropped
  // for negative values of offset (below the main diagonal)
  // "topmost rows" (along dim1) are dropped.
  // Note that we invert +/- in the second to absorb the negative
  // sign in the offset.
  if (offset >= 0) {
    diag_size = std::max<int64_t>(std::min(self.size(dim1), self.size(dim2)-offset), 0);
  } else {
    diag_size = std::max<int64_t>(std::min(self.size(dim1)+offset, self.size(dim2)), 0);
  }
#ifndef USE_TH_SIZE_ZERO_DIM
  AT_CHECK(diag_size > 0, "invalid diagonal offset ", offset); // the diagonal offset was too large in magnitude
#endif

  // NumPy allows you to specify offsets "off the end"; let's just be careful not to
  // set a ridiculous storage_offset in that case (technically it shouldn't matter
  // because there are no elements in the tensor, but let's be kosher).
  if (diag_size == 0) {
    // skip
  } else if (offset >= 0) {
    storage_offset += offset * self.stride(dim2);
  } else {
    storage_offset -= offset * self.stride(dim1);
  }

  // construct new size and stride: we drop dim1 and dim2 (maximum first for not changing the index of the minumum)
  // the new ("joint") dimension is appended to the end of the shape / stride to match numpy semantics
  auto sizes = std::vector<int64_t>(self.sizes());
  auto strides = std::vector<int64_t>(self.strides());
  sizes.erase(sizes.begin() + std::max(dim1, dim2));
  strides.erase(strides.begin() + std::max(dim1, dim2));
  sizes.erase(sizes.begin() + std::min(dim1, dim2));
  strides.erase(strides.begin() + std::min(dim1, dim2));
  sizes.push_back(diag_size);
  strides.push_back(self.stride(dim1)+self.stride(dim2));

  // return view with new parameters
  return self.as_strided(sizes, strides, storage_offset);
}

Tensor expand(const Tensor& self, IntList size, bool implicit) {
  // [expand implicit]
  // The implicit flag is set to true for any expand calls inserted by broadcast
  // operators in ExpandUtils.h This flag is recorded by the tracer to
  // distinguish between expands inserted by broadcasts and those explicitly
  // requested by the user, because it is legal to remove implicit expands
  // from the graph, but not legal to remove the explicit ones.
  if (size.size() < (size_t)self.dim()) {
    std::ostringstream ss;
    ss << "expand(" << self.type() << "{" << self.sizes() << "}, size=" << size
       << "): the number of sizes provided (" << size.size() << ") "
       << "must be greater or equal to the number of dimensions in the tensor ("
       << self.dim() << ")";
    throw std::runtime_error(ss.str());
  }

  std::vector<int64_t> expandedSizes;
  std::vector<int64_t> expandedStrides;
  std::tie(expandedSizes, expandedStrides) = inferExpandGeometry(self, size);

  return self.as_strided(expandedSizes, expandedStrides);
}

Tensor expand_as(const Tensor& self, const Tensor& other) {
  return self.expand(other.sizes());
}

Tensor as_strided(const Tensor& self, IntList size, IntList stride) {
  return self.as_strided(size, stride, self.storage_offset());
}

Tensor &as_strided_(Tensor& self, IntList size, IntList stride) {
  return self.as_strided_(size, stride, self.storage_offset());
}

Tensor narrow(const Tensor& self, int64_t dim, int64_t start, int64_t length) {
  AT_CHECK(self.dim() > 0, "narrow() cannot be applied to a 0-dim tensor.");
  auto cur_size = self.size(dim);
  if (start < 0) {
    AT_ERROR("start out of range");
  }
#ifndef USE_TH_SIZE_ZERO_DIM
  if (length <= 0 || start > cur_size - length) {
#else
  if (length < 0 || start > cur_size - length) {
#endif
    AT_ERROR("start (", start, ") + length (", length, ") exceeds dimension size (", cur_size, ").");
  }
  return at::slice(self, dim, start, start + length, 1);
}

Tensor permute(const Tensor& self, IntList dims) {
  auto nDims = self.dim();
  if (dims.size() != (size_t)nDims) {
    AT_ERROR("number of dims don't match in permute");
  }
  auto oldSizes = self.sizes();
  auto oldStrides = self.strides();
  std::vector<int64_t> newSizes(nDims);
  std::vector<int64_t> newStrides(nDims);
  std::vector<bool> seen(nDims);
  for (int64_t i = 0; i < nDims; i++) {
    auto dim = maybe_wrap_dim(dims[i], nDims);
    if (seen[dim]) {
      AT_ERROR("repeated dim in permute");
    }
    seen[dim] = true;
    newSizes[i] = oldSizes[dim];
    newStrides[i] = oldStrides[dim];
  }
  return self.as_strided(newSizes, newStrides);
}

Tensor repeat(const Tensor& self, IntList repeats) {
  if (repeats.size() < (size_t)self.dim()) {
    AT_ERROR("Number of dimensions of repeat dims can not be smaller than number of dimensions of tensor");
  }

  // Add new leading dimensions to the tensor if the
  // number of target dimensions is larger than the
  // number of source dimensions.
  int64_t num_new_dimensions = repeats.size() - self.dim();
  std::vector<int64_t> padded_size(num_new_dimensions, 1);
  padded_size.insert(padded_size.end(), self.sizes().begin(), self.sizes().end());
  std::vector<int64_t> target_size(repeats.size());
  for(size_t idx = 0; idx < repeats.size(); ++idx) {
    target_size[idx] = padded_size[idx] * repeats[idx];
  }

  Tensor xtensor = self.expand(padded_size);

  Tensor result = self.type().tensor(target_size);
  Tensor urtensor = result.type().alias(result);
  for (int64_t i = 0; i < xtensor.dim(); ++i) {
    // can't unfold with step 0, so make sure step is at least 1
    // (it doesn't matter what it is in that case, because the size is 0).
    urtensor = urtensor.unfold(i, xtensor.size(i), std::max<int64_t>(xtensor.size(i), 1));
  }

  urtensor.copy_(xtensor.expand_as(urtensor));

  return result;
}

// Infers the size of a dim with size -1, if it exists. Also checks that new
// shape is compatible with the number of elements.
static std::vector<int64_t> infer_size(IntList shape, int64_t numel) {
  auto res = shape.vec();
  int64_t newsize = 1;
  auto infer_dim = at::optional<int64_t>();
  for (int64_t dim = 0, ndim = shape.size(); dim != ndim; dim++) {
    if (shape[dim] == -1) {
      if (infer_dim) {
        throw std::runtime_error("only one dimension can be inferred");
      }
      infer_dim = dim;
    } else if (shape[dim] >= 0) {
      newsize *= shape[dim];
    } else {
      AT_ERROR("invalid shape dimension ", shape[dim]);
    }
  }

  if (numel == newsize || (infer_dim && newsize > 0 && numel % newsize == 0)) {
    if (infer_dim) {
      // we have a degree of freedom here to select the dimension size; follow NumPy semantics
      // and just bail.
      AT_CHECK(newsize != 0, "cannot reshape tensor of 0 elements into shape ", shape);
      res[*infer_dim] = numel / newsize;
    }
#ifndef USE_TH_SIZE_ZERO_DIM
    if (numel == 0) {
      // Collapse zero-element shapes into one dimension because TH handles zeros
      // in sizes strangely: x.resize_(1, 0) has shape (1,). TODO: remove this
      // once we have multi-dimensional empty tensors.
      return {0};
    }
#endif
    return res;
  }

  std::ostringstream ss;
  ss << "shape '" << shape << "' is invalid for input of size " << numel;
  throw std::runtime_error(ss.str());
}

Tensor reshape(const Tensor& self, IntList proposed_shape) {
  if (self.type().is_sparse()) {
    AT_ERROR("reshape is not implemented for sparse tensors");
  }
  auto shape = infer_size(proposed_shape, self.numel());
  if (auto stride = THTensor_compute_stride(self.sizes(), self.strides(), shape)) {
    return self.as_strided(shape, *stride);
  }
  return at::_unsafe_view(self.clone(), shape);
}

Tensor reshape_as(const Tensor& self, const Tensor& other) {
  return self.reshape(other.sizes());
}

Tensor select(const Tensor& self, int64_t dim, int64_t index) {
  int64_t ndim = self.dim();
  AT_CHECK(ndim > 0, "select() cannot be applied to a 0-dim tensor.");
  dim = maybe_wrap_dim(dim, ndim);
  auto size = self.size(dim);
  if (index < -size || index >= size) {
    std::stringstream ss;
    ss << "select(): index " << index << " out of range for tensor of size ";
    ss << self.sizes() << " at dimension " << dim;
    throw std::runtime_error(ss.str());
  }
  if (index < 0) {
    index += size;
  }
  auto sizes = std::vector<int64_t>(self.sizes());
  auto strides = std::vector<int64_t>(self.strides());
  auto storage_offset = self.storage_offset() + index * strides[dim];
  sizes.erase(sizes.begin() + dim);
  strides.erase(strides.begin() + dim);
  return self.as_strided(sizes, strides, storage_offset);
}

Tensor slice(const Tensor& self, int64_t dim, int64_t start, int64_t end, int64_t step) {
  int64_t ndim = self.dim();
  AT_CHECK(ndim > 0, "slice() cannot be applied to a 0-dim tensor.");
  dim = maybe_wrap_dim(dim, ndim);
  auto sizes = std::vector<int64_t>(self.sizes());
  auto strides = std::vector<int64_t>(self.strides());
  if (step <= 0) {
    // TODO: support negative strides
    throw std::runtime_error("slice step must be positive");
  }
  if (start < 0) {
    start += sizes[dim];
  }
  if (end < 0) {
    end += sizes[dim];
  }
  if (start < 0) {
    start = 0;
  } else if (start >= sizes[dim]) {
    start = sizes[dim];
  }
  if (end < start) {
    end = start;
  } else if (end >= sizes[dim]) {
    end = sizes[dim];
  }
  auto storage_offset = self.storage_offset() + start * strides[dim];
  auto len = end - start;
#ifndef USE_TH_SIZE_ZERO_DIM
  if (len == 0) {
    // TODO: currently we don't have support for 0-sized dims, return size 0 tensor for now
    return self.type().tensor();
  }
#endif
  sizes[dim] = (len + step - 1) / step;  // round-up
  strides[dim] *= step;
  return self.as_strided(sizes, strides, storage_offset);
}

std::vector<Tensor> split(const Tensor& self, int64_t split_size, int64_t dim) {
  AT_CHECK(self.dim() != 0, "split expects at least a 1-dimensional tensor");
  AT_CHECK(split_size >= 0,  "split expects split_size be non-negative, but got split_size=", split_size);
  int64_t dim_size = self.size(dim);
  AT_CHECK(split_size > 0 || self.size(dim) == 0,
           "split_size can only be 0 if dimension size is 0, "
           "but got dimension size of ", dim_size);
  // if split_size is 0 and dimension size is 0, there is 1 split.
  int64_t num_splits = 1;
  if (split_size != 0) {
    // ensuring num_splits is at least 1 makes consistent the case where split_size > dim_size
    // (returns a single split).  We might want to error here, but keep it for BC.
    num_splits = std::max<int64_t>((dim_size + split_size - 1) / split_size, 1);
  }
  std::vector<Tensor> splits(num_splits);
  int64_t last_split_size = split_size - (split_size * num_splits - dim_size);

  for (int64_t i = 0; i < num_splits; ++i) {
    auto length = i < num_splits - 1 ? split_size : last_split_size;
    splits[i] = self.narrow(dim, i * split_size, length);
  }
  return splits;
}

std::vector<Tensor> split_with_sizes(const Tensor& self, IntList split_sizes, int64_t dim) {
  AT_CHECK(self.dim() != 0, "split expects at least a 1-dimensional tensor");
  int64_t dim_size = self.size(dim);
  int64_t num_splits = split_sizes.size();
  std::vector<Tensor> splits(num_splits);
  int64_t start_idx = 0;
  int64_t i;

  for (i = 0; i < num_splits; ++i) {
    auto length = split_sizes[i];
    if (length < 0) {
      std::ostringstream ss;
      ss << "split_with_sizes expects split_sizes have only non-negative "
         << "entries, but got split_sizes=" << split_sizes;
      throw std::runtime_error(ss.str());
    }
    splits[i] = self.narrow(dim, start_idx, length);
    start_idx += length;
  }
  if (start_idx != dim_size) {
    std::ostringstream ss;
    ss << "split_with_sizes expects split_sizes to sum exactly to "
       << dim_size << " (input tensor's size at dimension " << dim << "), "
       << "but got split_sizes=" << split_sizes;
    throw std::runtime_error(ss.str());
  }
  return splits;
}

static inline std::vector<Tensor> get_stack_inputs(TensorList tensors, int64_t dim) {
  std::vector<Tensor> inputs(tensors.size());
  for (size_t i = 0; i < tensors.size(); ++i) {
    inputs[i] = tensors[i].unsqueeze(dim);
  }
  return inputs;
}

Tensor stack(TensorList tensors, int64_t dim) {
  if (tensors.size() == 0) {
    throw std::runtime_error("stack expects a non-empty TensorList");
  }
  dim = maybe_wrap_dim(dim, tensors[0].dim() + 1);
  return at::cat(get_stack_inputs(tensors, dim), dim);
}

Tensor& stack_out(Tensor& result, TensorList tensors, int64_t dim) {
  if (tensors.size() == 0) {
    throw std::runtime_error("stack expects a non-empty TensorList");
  }
  dim = maybe_wrap_dim(dim, tensors[0].dim() + 1);
  return at::cat_out(result, get_stack_inputs(tensors, dim), dim);
}

static inline Tensor & sparse_transpose_(Tensor & self, int64_t dim0, int64_t dim1) {
  int64_t nsparseDims = self._sparseDims();
  if (dim0 >= nsparseDims || dim1 >= nsparseDims) {
    AT_ERROR(
        "sparse transpose: transposed dimensions must be sparse ",
        "Got sparseDims: ", nsparseDims, ", d0: ", dim0, ", d1: ", dim1);
  }

  if (self._indices().numel() == 0 && self._values().numel() == 0) {
    std::vector<int64_t> sizes(self.sizes());
    std::swap(sizes[dim0], sizes[dim1]);

    return self.sparse_raw_resize_(sizes, self._sparseDims(), self._denseDims());
  } else {
    auto indices = self._indices();
    auto row0 = indices.select(0, dim0);
    auto row1 = indices.select(0, dim1);

    // swap row0 and row1
    auto tmp = at::zeros_like(row0);
    tmp.copy_(row0);
    row0.copy_(row1);
    row1.copy_(tmp);

    std::vector<int64_t> sizes(self.sizes());
    std::swap(sizes[dim0], sizes[dim1]);

    return self.sparse_raw_resize_(sizes, -1, -1);
  }
}

Tensor & transpose_(Tensor & self, int64_t dim0, int64_t dim1) {
  auto ndims = self.dim();
  dim0 = maybe_wrap_dim(dim0, ndims);
  dim1 = maybe_wrap_dim(dim1, ndims);
  if (dim0 == dim1) {
    return self;
  }

  if (self.is_sparse()) {
    return sparse_transpose_(self, dim0, dim1);
  }

  std::vector<int64_t> strides(self.strides());
  std::vector<int64_t> sizes(self.sizes());
  std::swap(strides[dim0], strides[dim1]);
  std::swap(sizes[dim0], sizes[dim1]);
  return self.as_strided_(sizes, strides);
}

Tensor transpose(const Tensor & self, int64_t dim0, int64_t dim1) {
  auto ndims = self.dim();
  dim0 = maybe_wrap_dim(dim0, ndims);
  dim1 = maybe_wrap_dim(dim1, ndims);
  if (dim0 == dim1) {
    return self;
  }

  if (self.is_sparse()) {
    Tensor self_clone = self.clone();  // yes, this is what THS does
    return sparse_transpose_(self_clone, dim0, dim1);
  }

  std::vector<int64_t> strides(self.strides());
  std::vector<int64_t> sizes(self.sizes());
  std::swap(strides[dim0], strides[dim1]);
  std::swap(sizes[dim0], sizes[dim1]);
  return self.as_strided(sizes, strides);
}

static void check_t(const Tensor& self, const char *fn) {
  if (self.is_sparse()) {
    int64_t sparseDims = self._sparseDims();
    int64_t denseDims = self._denseDims();
    if (!(sparseDims == 2 && denseDims == 0)) {
      AT_ERROR(fn, " expects a tensor with 2 sparse and 0 dense dimensions, but got ",
               sparseDims, " sparse and ", denseDims, " dense dimensions");
    }
  } else if (self.dim() != 2) {
    AT_ERROR(fn, " expects a 2D tensor, but self is ", self.dim(), "D");
  }
}

Tensor t(const Tensor & self) {
  check_t(self, "t()");
  return self.transpose(0, 1);
}

Tensor & t_(Tensor & self) {
  check_t(self, "t_()");
  return self.transpose_(0, 1);
}

std::tuple<std::vector<int64_t>, std::vector<int64_t> >
inferSqueezeGeometry(const Tensor &tensor) {
  std::vector<int64_t> sizes;
  std::vector<int64_t> strides;

  for(int64_t d = 0; d < tensor.dim(); d++) {
    if(tensor.sizes()[d] != 1) {
      sizes.push_back(tensor.sizes()[d]);
      strides.push_back(tensor.strides()[d]);
    }
  }

  return std::make_tuple(sizes, strides);
}

std::tuple<std::vector<int64_t>, std::vector<int64_t> >
inferSqueezeGeometry(const Tensor& tensor, int64_t dim) {
  std::vector<int64_t> sizes;
  std::vector<int64_t> strides;

  for(int64_t d = 0; d < tensor.dim(); d++) {
    if(d != dim || tensor.sizes()[dim] != 1) {
      sizes.push_back(tensor.sizes()[d]);
      strides.push_back(tensor.strides()[d]);
    }
  }
  return std::make_tuple(sizes, strides);
}

std::tuple<std::vector<int64_t>, std::vector<int64_t> >
inferUnsqueezeGeometry(const Tensor& tensor, int64_t dim) {
#ifndef USE_TH_SIZE_ZERO_DIM
  if (tensor.numel() == 0) {
    throw std::runtime_error("cannot unsqueeze empty tensor");
  }
#endif
  std::vector<int64_t> sizes(tensor.sizes());
  std::vector<int64_t> strides(tensor.strides());
  int64_t new_stride = dim >= tensor.dim() ? 1 : sizes[dim] * strides[dim];
  sizes.insert(sizes.begin() + dim, 1);
  strides.insert(strides.begin() + dim, new_stride);

  return std::make_tuple(sizes, strides);
}

Tensor squeeze(const Tensor& self) {
  auto g = inferSqueezeGeometry(self);
  return self.as_strided(std::get<0>(g), std::get<1>(g));
}

Tensor squeeze(const Tensor& self, int64_t dim) {
  int64_t dims = self.dim();
  dim = maybe_wrap_dim(dim, dims);

  if (dims == 0 || self.sizes()[dim] != 1) {
    return self.as_strided(self.sizes().vec(), self.strides().vec());
  }
  auto g = inferSqueezeGeometry(self, dim);
  return self.as_strided(std::get<0>(g), std::get<1>(g));
}

Tensor & squeeze_(Tensor& self) {
  auto g = inferSqueezeGeometry(self);
  return self.as_strided_(std::get<0>(g), std::get<1>(g));
}

Tensor & squeeze_(Tensor& self, int64_t dim) {
  int64_t dims = self.dim();
  dim = maybe_wrap_dim(dim, self.dim());

  if (dims == 0 || self.sizes()[dim] != 1) {
    return self.as_strided_(self.sizes().vec(), self.strides().vec());
  }
  auto g = inferSqueezeGeometry(self, dim);
  return self.as_strided_(std::get<0>(g), std::get<1>(g));
}

// _unsafe_view() differs from view() in that the returned tensor isn't treated
// as a view for the purposes of automatic differentiation. (It's not listed in
// VIEW_FUNCTIONS in gen_autograd.py).  It's only safe to use if the `self` tensor
// is temporary. For example, the viewed tensor here (a + b) is discarded immediately
// after viewing:
//
//  res = at::_unsafe_view(a + b, size);
//
// This is a hack because in-place operations on tensors treated like views
// can be much more expensive than the same operations on non-view tensors.
Tensor _unsafe_view(const Tensor& self, IntList size) {
  return self.view(size);
}

Tensor unsqueeze(const Tensor& self, int64_t dim) {
  dim = maybe_wrap_dim(dim, self.dim() + 1);

  auto g = inferUnsqueezeGeometry(self, dim);
  return self.as_strided(std::get<0>(g), std::get<1>(g));
}

Tensor & unsqueeze_(Tensor& self, int64_t dim) {
  dim = maybe_wrap_dim(dim, self.dim() + 1);

  auto g = inferUnsqueezeGeometry(self, dim);
  return self.as_strided_(std::get<0>(g), std::get<1>(g));
}

Tensor flatten(const Tensor& self, int64_t start_dim, int64_t end_dim) {
  start_dim = maybe_wrap_dim(start_dim, self.dim());
  end_dim = maybe_wrap_dim(end_dim, self.dim());
  AT_CHECK(start_dim <= end_dim, "flatten() has invalid args: start_dim cannot come after end_dim");

  if (start_dim == end_dim) {
    return self;
  }

  // We don't want to infer_size on the entire shape, because that can give us an extra degree
  // of freedom we don't want; for example, consider shape [0, 1, 3, 0], with start_dim=1, end_dim=2.
  // It's clear we want result shape [0, 3, 0] but passing [0, -1, 0] to infer_size means the -1
  // can take on any value and satisfy the constraints.
  auto slice_numel = prod_intlist(self.sizes().slice(start_dim, end_dim - start_dim + 1));
  std::vector<int64_t> shape;
  shape.reserve(self.dim() - end_dim + start_dim);
  for (int64_t i = 0; i < start_dim; i++) {
    shape.push_back(self.size(i));
  }
  shape.push_back(slice_numel);
  for (int64_t i = end_dim + 1; i < self.dim(); i++) {
    shape.push_back(self.size(i));
  }

  return self.reshape(shape);
}

Tensor view_as(const Tensor& self, const Tensor& other) {
  return self.view(other.sizes());
}

int64_t numel(const Tensor& self) {
  return self.pImpl->numel();
}

std::vector<Tensor> unbind(const Tensor &self, int64_t dim) {
  dim = maybe_wrap_dim(dim, self.dim());
  int64_t size = self.size(dim);
  std::vector<Tensor> tensors(size);
  for (int i = 0; i < size; i++) {
    tensors[i] = self.select(dim, i);
  }
  return tensors;
}

std::vector<Tensor> meshgrid(TensorList tensors) {
  int64_t size = tensors.size();
  AT_CHECK(size > 0, "meshgrid expects a non-empty TensorList");
  std::vector<int64_t> shape(size);
  for(int64_t i = 0; i < size; i++) {
    switch (tensors[i].dim()) {
    case 0:
      shape[i] = 1;
      break;
    case 1:
      shape[i] = tensors[i].size(0);
      break;
    default:
      AT_ERROR("Expected scalar or 1D tensor in the tensor list but got: ", tensors[i]);
    }
  }
  std::vector<Tensor> grids;
  for(int64_t i = 0; i < size; i++) {
    std::vector<int64_t> view_shape(size, 1);
    view_shape[i] = -1;
    grids.push_back(tensors[i].view(view_shape).expand(shape));
  }
  return grids;
}

}
}
