/////////////////////////////////////////////////////////////////////////////
//
// BSD 3-Clause License
//
// Copyright (c) 2019, The Regents of the University of California
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////////

#include "gpuSolver.h"

namespace gpl {
using namespace std;
using utl::GPL;

void GpuSolver::cudaerror(cudaError_t code)
{
  if (code != cudaSuccess) {
    log_->error(GPL,
                1,
                "[CUDA ERROR] {} at line {} in file {} \n",
                cudaGetErrorString(code),
                __LINE__,
                __FILE__);
  }
}
void GpuSolver::cusparseerror(cusparseStatus_t code)
{
  if (code != CUSPARSE_STATUS_SUCCESS) {
    log_->error(GPL,
                1,
                "[CUSPARSE ERROR] {} at line {} in file {}\n",
                cusparseGetErrorString(code),
                __LINE__,
                __FILE__);
  }
}

void GpuSolver::cusolvererror(cusolverStatus_t code)
{
  if (code != CUSOLVER_STATUS_SUCCESS) {
    log_->error(GPL,
                1,
                "[CUSOLVER ERROR] {} at line {} in file {}\n",
                cudaGetErrorString(*(cudaError_t*) &code),
                __LINE__,
                __FILE__);
  }
}

GpuSolver::GpuSolver()
{
}

GpuSolver::GpuSolver(SMatrix& placeInstForceMatrix,
                     Eigen::VectorXf& fixedInstForceVec,
                     utl::Logger* logger)
{
  // {cooRowIndex_, cooColIndex_, cooVal_} are the host vectors used to store
  // the sparse format of placeInstForceMatrix.
  nnz_ = placeInstForceMatrix.nonZeros();
  vector<int> cooRowIndex, cooColIndex;
  vector<float> cooVal;
  cooRowIndex.reserve(nnz_);
  cooColIndex.reserve(nnz_);
  cooVal.reserve(nnz_);

  for (int row = 0; row < placeInstForceMatrix.outerSize(); row++) {
    for (typename Eigen::SparseMatrix<float, Eigen::RowMajor>::InnerIterator it(
             placeInstForceMatrix, row);
         it;
         ++it) {
      cooRowIndex.push_back(it.row());
      cooColIndex.push_back(it.col());
      cooVal.push_back(it.value());
    }
  }

  m_ = fixedInstForceVec.size();
  nnz_ = cooVal.size();
  log_ = logger;
  d_cooRowIndex_.resize(nnz_);
  d_cooColIndex_.resize(nnz_);
  d_cooVal_.resize(nnz_);
  d_fixedInstForceVec_.resize(m_);
  d_instLocVec_.resize(m_);

  // Copy the COO formatted triplets to device
  thrust::copy(cooRowIndex.begin(), cooRowIndex.end(), d_cooRowIndex_.begin());
  thrust::copy(cooColIndex.begin(), cooColIndex.end(), d_cooColIndex_.begin());
  thrust::copy(cooVal.begin(), cooVal.end(), d_cooVal_.begin());
  thrust::copy(&fixedInstForceVec[0],
               &fixedInstForceVec[m_ - 1],
               d_fixedInstForceVec_.begin());

  // Set raw pointers to point to the triplets in the device
  r_cooRowIndex_ = thrust::raw_pointer_cast(d_cooRowIndex_.data());
  r_cooColIndex_ = thrust::raw_pointer_cast(d_cooColIndex_.data());
  r_cooVal_ = thrust::raw_pointer_cast(d_cooVal_.data());
  r_fixedInstForceVec_ = thrust::raw_pointer_cast(d_fixedInstForceVec_.data());
  r_instLocVec_ = thrust::raw_pointer_cast(d_instLocVec_.data());
}

void GpuSolver::cusolverCal(Eigen::VectorXf& instLocVec)
{
  // Parameters that don't change with iteration and used in the CUDA code
  const float tol = 1e-6;  // 	Tolerance to decide if singular or not.
  const int reorder = 0;   // "0" for common matrix without ordering
  int singularity = -1;    // Output. -1 = A means invertible

  // Set handler
  cusolverSpHandle_t handleCusolver = NULL;
  cusparseHandle_t handleCusparse = NULL;
  cudaStream_t stream = NULL;

  // Initialize handler
  cusolvererror(cusolverSpCreate(&handleCusolver));
  cusparseerror(cusparseCreate(&handleCusparse));
  cudaerror(cudaStreamCreate(&stream));
  cusolvererror(cusolverSpSetStream(handleCusolver, stream));
  cusparseerror(cusparseSetStream(handleCusparse, stream));

  // Create and define cusparse descriptor
  cusparseMatDescr_t descr = NULL;
  cusparseerror(cusparseCreateMatDescr(&descr));
  cusparseerror(cusparseSetMatType(descr, CUSPARSE_MATRIX_TYPE_GENERAL));
  cusparseerror(cusparseSetMatIndexBase(descr, CUSPARSE_INDEX_BASE_ZERO));

  // transform from coordinates (COO) values to compressed row pointers (CSR)
  // values https://docs.nvidia.com/cuda/cusparse/index.html
  int* r_csrRowInd = NULL;
  thrust::device_vector<int> d_csrRowInd(m_ + 1, 0);
  r_csrRowInd = thrust::raw_pointer_cast(d_csrRowInd.data());

  cusparseerror(cusparseXcoo2csr(handleCusparse,
                                 r_cooRowIndex_,
                                 nnz_,
                                 m_,
                                 r_csrRowInd,
                                 CUSPARSE_INDEX_BASE_ZERO));

  cusolvererror(cusolverSpScsrlsvqr(handleCusolver,
                                    m_,
                                    nnz_,
                                    descr,
                                    r_cooVal_,
                                    r_csrRowInd,
                                    r_cooColIndex_,
                                    r_fixedInstForceVec_,
                                    tol,
                                    reorder,
                                    r_instLocVec_,
                                    &singularity));

  // Sync and Copy data to host
  cudaerror(cudaMemcpyAsync(instLocVec.data(),
                            r_instLocVec_,
                            sizeof(float) * m_,
                            cudaMemcpyDeviceToHost,
                            stream));

  // cudaerror(cudaFree(r_csrRowInd));
  cusparseerror(cusparseDestroyMatDescr(descr));
  cusparseerror(cusparseDestroy(handleCusparse));
  cusolvererror(cusolverSpDestroy(handleCusolver));
}

__global__ void Multi_MatVec(float* r_error_,
                             int nnz_,
                             int m_,
                             float* r_Ax,
                             float* r_fixedInstForceVec_,
                             float* r_instLocVec_,
                             int* r_cooRowIndex_,
                             int* r_cooColIndex_,
                             float* r_cooVal_)
{
  int num = blockIdx.x * blockDim.x + threadIdx.x;
  if (num < nnz_) {
    r_Ax[r_cooRowIndex_[num]]
        += r_cooVal_[num] * r_instLocVec_[r_cooColIndex_[num]];
  }
  float sum = 0;
  for (int row = 0; row < m_; row++) {
    sum += (r_fixedInstForceVec_[row] > 0) ? r_fixedInstForceVec_[row]
                                           : -r_fixedInstForceVec_[row];
    if (r_fixedInstForceVec_[row] > r_Ax[row])
      r_error_[0] += r_fixedInstForceVec_[row] - r_Ax[row];
    else
      r_error_[0] -= r_fixedInstForceVec_[row] - r_Ax[row];
  }
  if (sum != 0)
    r_error_[0] = r_error_[0] / sum;
}

float GpuSolver::error()
{
  float* r_error_;
  thrust::device_vector<float> d_error_(1, 0);
  r_error_ = thrust::raw_pointer_cast(d_error_.data());
  float* r_Ax;
  thrust::device_vector<float> d_Ax(m_, 0);
  // thrust::fill(d_Ax.begin(), d_Ax.end(), 0);
  r_Ax = thrust::raw_pointer_cast(d_Ax.data());

  unsigned int threads = 512;
  unsigned int blocks = (m_ + threads - 1) / threads;
  Multi_MatVec<<<blocks, threads>>>(r_error_,
                                    nnz_,
                                    m_,
                                    r_Ax,
                                    r_fixedInstForceVec_,
                                    r_instLocVec_,
                                    r_cooRowIndex_,
                                    r_cooColIndex_,
                                    r_cooVal_);
  cudaerror(
      cudaMemcpy(&error_, r_error_, sizeof(float) * 1, cudaMemcpyDeviceToHost));
  return (error_ > 0) ? error_ : -error_;
}

GpuSolver::~GpuSolver()
{
}
}  // namespace gpl
