#include <torch/script.h>

#include "compat.h"
#include "index_info.h"

#include <vector>

#define CHECK_CPU(x) AT_ASSERTM(x.device().is_cpu(), #x " must be CPU tensor")

enum ReductionType { SUM, MEAN, MIN, MAX };

const std::map<std::string, ReductionType> reduce2REDUCE = {
    {"sum", SUM}, {"add", SUM}, {"mean", MEAN}, {"min", MIN}, {"max", MAX},
};

#define AT_DISPATCH_REDUCTION_TYPES(reduce, ...)                               \
  [&] {                                                                        \
    switch (reduce2REDUCE.at(reduce)) {                                        \
    case SUM: {                                                                \
      const ReductionType REDUCE = SUM;                                        \
      return __VA_ARGS__();                                                    \
    }                                                                          \
    case MEAN: {                                                               \
      const ReductionType REDUCE = MEAN;                                       \
      return __VA_ARGS__();                                                    \
    }                                                                          \
    case MIN: {                                                                \
      const ReductionType REDUCE = MIN;                                        \
      return __VA_ARGS__();                                                    \
    }                                                                          \
    case MAX: {                                                                \
      const ReductionType REDUCE = MAX;                                        \
      return __VA_ARGS__();                                                    \
    }                                                                          \
    }                                                                          \
  }()

template <typename scalar_t, ReductionType REDUCE> struct Reducer {
  static inline scalar_t init() {
    if (REDUCE == MIN) {
      return std::numeric_limits<scalar_t>::max();
    } else if (REDUCE == MAX) {
      return std::numeric_limits<scalar_t>::lowest();
    } else {
      return (scalar_t)0;
    }
  }

  static inline void update(scalar_t *val, scalar_t new_val, int64_t *arg,
                            int64_t new_arg) {
    if (REDUCE == SUM || REDUCE == MEAN) {
      *val = *val + new_val;
    } else if ((REDUCE == MIN && new_val < *val) ||
               (REDUCE == MAX && new_val > *val)) {
      *val = new_val;
      *arg = new_arg;
    }
  }

  static inline void write(scalar_t *address, scalar_t val,
                           int64_t *arg_address, int64_t arg, int count) {
    if (REDUCE == SUM) {
      *address = val;
    } else if (REDUCE == MEAN) {
      *address = val / (count > 0 ? count : (scalar_t)1);
    } else if (REDUCE == MIN || REDUCE == MAX) {
      if (count > 0) {
        *address = val;
        *arg_address = arg;
      } else {
        *address = (scalar_t)0;
      }
    }
  }
};

std::tuple<torch::Tensor, torch::optional<torch::Tensor>>
segment_csr(torch::Tensor src, torch::Tensor indptr,
            torch::optional<torch::Tensor> out_opt, std::string reduce) {
  CHECK_CPU(src);
  CHECK_CPU(indptr);
  if (out_opt.has_value())
    CHECK_CPU(out_opt.value());

  AT_ASSERTM(src.dim() >= indptr.dim(), "Input mismatch");

  // Broadcasting `indptr` via `expand`.
  auto sizes = indptr.sizes().vec();
  for (int i = 0; i < indptr.dim() - 1; i++) {
    sizes[i] = src.size(i);
  }
  indptr = indptr.expand(sizes);

  src = src.contiguous();
  auto reduce_dim = indptr.dim() - 1;

  torch::Tensor out;
  if (out_opt.has_value()) {
    out = out_opt.value().contiguous();
    for (int i = 0; i < out.dim(); i++)
      if (i != reduce_dim)
        AT_ASSERTM(src.size(i) == out.size(i), "Input mismatch");
    AT_ASSERTM(out.size(reduce_dim) == indptr.size(reduce_dim) - 1,
               "Input mismatch");
  } else {
    sizes = src.sizes().vec();
    sizes[reduce_dim] = indptr.size(reduce_dim) - 1;
    out = torch::empty(sizes, src.options());
  }

  torch::optional<torch::Tensor> arg_out = torch::nullopt;
  int64_t *arg_out_data = nullptr;
  if (reduce2REDUCE.at(reduce) == MIN || reduce2REDUCE.at(reduce) == MAX) {
    arg_out = torch::full_like(out, src.size(reduce_dim), indptr.options());
    arg_out_data = arg_out.value().DATA_PTR<int64_t>();
  }

  auto N = out.size(reduce_dim) * (indptr.numel() / indptr.size(-1));
  auto K = out.numel() / N;
  auto E = src.size(reduce_dim);

  auto indptr_info = getTensorInfo<int64_t>(indptr);
  auto stride = indptr_info.strides[indptr_info.dims - 1];
  AT_DISPATCH_ALL_TYPES(src.scalar_type(), "segment_csr", [&] {
    auto src_data = src.DATA_PTR<scalar_t>();
    auto out_data = out.DATA_PTR<scalar_t>();

    std::vector<scalar_t> vals(K);
    int64_t row_start, row_end;
    std::vector<int64_t> args(K);
    AT_DISPATCH_REDUCTION_TYPES(reduce, [&] {
      for (int n = 0; n < N; n++) {
        int offset = IndexPtrToOffset<int64_t>::get(n, indptr_info);
        row_start = indptr_info.data[offset];
        row_end = indptr_info.data[offset + stride];

        offset = (n / (indptr.size(-1) - 1)) * E * K;
        for (int k = 0; k < K; k++) {
          vals[k] = Reducer<scalar_t, REDUCE>::init();
        }
        for (int64_t e = row_start; e < row_end; e++) {
          for (int k = 0; k < K; k++) {
            Reducer<scalar_t, REDUCE>::update(
                &vals[k], src_data[offset + e * K + k], &args[k], e);
          }
        }
        for (int k = 0; k < K; k++) {
          Reducer<scalar_t, REDUCE>::write(out_data + n * K + k, vals[k],
                                           arg_out_data + n * K + k, args[k],
                                           row_end - row_start);
        }
      }
    });
  });

  return std::make_tuple(out, arg_out);
}

std::tuple<torch::Tensor, torch::optional<torch::Tensor>>
segment_coo(torch::Tensor src, torch::Tensor index, torch::Tensor out,
            std::string reduce) {
  CHECK_CPU(src);
  CHECK_CPU(index);
  CHECK_CPU(out);

  AT_ASSERTM(src.dim() >= index.dim(), "Input mismatch");

  // Broadcasting `index` via `expand`.
  auto sizes = index.sizes().vec();
  for (int i = 0; i < index.dim(); i++) {
    sizes[i] = src.size(i);
  }
  index = index.expand(sizes);

  src = src.contiguous();
  out = out.contiguous();
  auto reduce_dim = index.dim() - 1;

  for (int i = 0; i < out.dim(); i++)
    if (i != reduce_dim)
      AT_ASSERTM(src.size(i) == out.size(i), "Input mismatch");

  torch::optional<torch::Tensor> arg_out = torch::nullopt;
  int64_t *arg_out_data = nullptr;
  if (reduce2REDUCE.at(reduce) == MIN || reduce2REDUCE.at(reduce) == MAX) {
    arg_out = torch::full_like(out, src.size(reduce_dim), index.options());
    arg_out_data = arg_out.value().DATA_PTR<int64_t>();
  }

  auto E_1 = index.numel() / src.size(reduce_dim);
  auto E_2 = src.size(reduce_dim);
  auto K = src.numel() / index.numel();
  auto N = out.size(reduce_dim);

  auto index_info = getTensorInfo<int64_t>(index);
  auto stride = index_info.strides[index_info.dims - 1];
  AT_DISPATCH_ALL_TYPES(src.scalar_type(), "segment_coo", [&] {
    auto src_data = src.DATA_PTR<scalar_t>();
    auto out_data = out.DATA_PTR<scalar_t>();

    std::vector<scalar_t> vals(K);
    int64_t idx, next_idx, row_start;
    std::vector<int64_t> args(K);
    AT_DISPATCH_REDUCTION_TYPES(reduce, [&] {
      for (int e_1 = 0; e_1 < E_1; e_1++) {
        int offset = IndexToOffset<int64_t>::get(e_1 * E_2, index_info);
        idx = index_info.data[offset];

        for (int k = 0; k < K; k++) {
          vals[k] = out_data[e_1 * N * K + k];
        }

        row_start = 0;
        for (int e_2 = 0; e_2 < E_2; e_2++) {

          for (int k = 0; k < K; k++) {
            Reducer<scalar_t, REDUCE>::update(
                &vals[k], src_data[e_1 * E_2 * K + e_2 * K + k], &args[k], e_2);
          }

          if (e_2 == E_2 - 1) {
            for (int k = 0; k < K; k++) {
              Reducer<scalar_t, REDUCE>::write(
                  out_data + e_1 * N * K + idx * K + k, vals[k],
                  arg_out_data + e_1 * N * K + idx * K + k, args[k],
                  e_2 + 1 - row_start);
            }
          } else {
            next_idx = index_info.data[offset + (e_2 + 1) * stride];
            assert(idx <= next_idx);

            if (idx != next_idx) {
              for (int k = 0; k < K; k++) {
                Reducer<scalar_t, REDUCE>::write(
                    out_data + e_1 * N * K + idx * K + k, vals[k],
                    arg_out_data + e_1 * N * K + idx * K + k, args[k],
                    e_2 + 1 - row_start);

                vals[k] = out_data[e_1 * N * K + next_idx * K + k];
              }
              row_start = e_2 + 1;
            }

            idx = next_idx;
          }
        }
      }
    });
  });

  return std::make_tuple(out, arg_out);
}

static auto registry =
    torch::RegisterOperators("torch_scatter_cpu::segment_csr", &segment_csr)
        .op("torch_scatter_cpu::segment_coo", &segment_coo);
