/*
 * Copyright (c) 2019, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <algorithm> 
#include <iostream> 
#include <vector> 
#include <chrono>
#include "common.h"

using namespace std; 

#define MAX_KERNEL_THREADS 256

// float or double 
typedef float vtype;
typedef vector<vector<vtype>> matrix; 

// Forward declarations
template<bool weighted, typename T>
class row_sum;

template<bool weighted, typename T>
class intersection;

template<bool weighted, typename T>
class jw;

template<bool weighted, typename T>
class fill_elements;

template<typename T, sycl::memory_scope MemoryScope = sycl::memory_scope::device>
static inline void atomicAdd(T& val, const T delta)
{
  sycl::ext::oneapi::atomic_ref<T, sycl::memory_order::relaxed, 
     MemoryScope, sycl::access::address_space::global_space> ref(val);
  ref.fetch_add(delta);
}

template<typename T>
T parallel_prefix_sum(nd_item<3> &item, const int n, const int *ind, global_ptr<T> w) 
{
  T sum = 0.0;
  T last;
  const int blockDim_x = item.get_local_range(2);
  const int threadIdx = item.get_local_id(2);
  auto sg = item.get_sub_group();

  //the number of nnz elements in a row is a multiple of blockDim_x.x
  int mn =((n+blockDim_x-1)/blockDim_x*blockDim_x); 

  for (int i=threadIdx; i<mn; i+=blockDim_x) {
    bool valid = i < n;
    last = sycl::select_from_group(sg, sum, blockDim_x-1);
    sum = (valid) ? w[ind[i]] : 0.0;

    for (int j=1; j<blockDim_x; j*=2) {
      T v = sycl::shift_group_right(sg, sum, j);
      if (threadIdx >= j) sum += v;
    }
    sum += last;
  }
  last = sycl::select_from_group(sg, sum, blockDim_x-1);
  return last;
}

// Volume of neighboors (*weight_s)
template<bool weighted, typename T>
void jaccard_row_sum(
  queue &q,
  const int n, 
  buffer<int, 1> &d_csrPtr, 
  buffer<int, 1> &d_csrInd, 
  buffer<T, 1> &d_weight_j, 
  buffer<T, 1> &d_work) 
{
  const int y = 4;
  range<3> sum_gws (1, (n+y-1)/y*y, 64/y);
  range<3> sum_lws (1, y, 64/y);

  q.submit([&] (handler &cgh) {
    auto csrPtr = d_csrPtr.get_access<sycl_read>(cgh);
    auto csrInd = d_csrInd.get_access<sycl_read>(cgh);
    auto w = d_weight_j.template get_access<sycl_read>(cgh);
    auto work = d_work.template get_access<sycl_discard_write>(cgh);
    cgh.parallel_for<class row_sum<weighted,T>>(nd_range<3>(sum_gws, sum_lws), [=] (nd_item<3> item) 
         [[sycl::reqd_sub_group_size(32)]] {

      for(int row = item.get_global_id(1); row < n; 
              row += item.get_group_range(1)*item.get_local_range(1)) {
        int start = csrPtr[row];
        int end   = csrPtr[row+1];
        int length= end-start;
        if (weighted) {
          T sum = parallel_prefix_sum(item, length, csrInd.get_pointer() + start, w.get_pointer()); 
          if (item.get_local_id(2) == 0) work[row] = sum;
        } else {
          work[row] = (T)length;
        }
      }
    });
  });
}

// Volume of intersections (*weight_i) and cumulated volume of neighboors (*weight_s)
// Note the number of columns is constrained by the number of rows
template<bool weighted, typename T>
void jaccard_is(
  queue &q,
  const int n,
  const int e, 
  buffer<int, 1> &d_csrPtr, 
  buffer<int, 1> &d_csrInd, 
  buffer<T, 1> &d_weight_j, 
  buffer<T, 1> &d_work, 
  buffer<T, 1> &d_weight_i, 
  buffer<T, 1> &d_weight_s)
{
  const int y = 4;
  range<3> is_gws ((n+7)/8*8, y, 32/y);
  range<3> is_lws(8, y, 32/y);

  q.submit([&] (handler &cgh) {
    auto csrPtr = d_csrPtr.get_access<sycl_read>(cgh);
    auto csrInd = d_csrInd.get_access<sycl_read>(cgh);
    auto weight_i = d_weight_i.template get_access<sycl_read_write>(cgh);
    auto weight_s = d_weight_s.template get_access<sycl_write>(cgh);
    auto v = d_weight_j.template get_access<sycl_read>(cgh);
    auto work = d_work.template get_access<sycl_read>(cgh);
    cgh.parallel_for<class intersection<weighted,T>>(nd_range<3>(is_gws, is_lws), [=] (nd_item<3> item) {

      for(int row = item.get_global_id(0); row < n; 
              row += item.get_group_range(0)*item.get_local_range(0)) {
        for (int j = csrPtr[row]+item.get_global_id(1); j < csrPtr[row+1]; 
                 j+= item.get_local_range(1) * item.get_group_range(1)) {

          int col = csrInd[j];
          //find which row has least elements (and call it reference row)
          int Ni = csrPtr[row+1] - csrPtr[row];
          int Nj = csrPtr[col+1] - csrPtr[col];
          int ref= (Ni < Nj) ? row : col;
          int cur= (Ni < Nj) ? col : row;

          //compute new sum weights
          weight_s[j] = work[row] + work[col];

          //compute new intersection weights 
          //search for the element with the same column index in the reference row
          for (int i = csrPtr[ref]+item.get_global_id(2); i < csrPtr[ref+1]; 
                   i += item.get_local_range(2) * item.get_group_range(2)) {
            int match  =-1;
            int ref_col = csrInd[i];
            T ref_val = weighted ? v[ref_col] : (T)1.0;

            //binary search (column indices are sorted within each row)
            int left = csrPtr[cur]; 
            int right= csrPtr[cur+1]-1; 
            while(left <= right){
              int middle = (left+right)>>1; 
              int cur_col= csrInd[middle];
              if (cur_col > ref_col) {
                right=middle-1;
              }
              else if (cur_col < ref_col) {
                left=middle+1;
              }
              else {
                match = middle; 
                break; 
              }
            }            

            //if the element with the same column index in the reference row has been found
            if (match != -1){
              atomicAdd(weight_i[j], ref_val);
            }
          }
        }
      }
    });
  });
}

template<bool weighted, typename T>
void jaccard_jw(
  queue &q,
  const int e, 
  buffer<T, 1> &d_csrVal, 
  const T gamma, 
  buffer<T, 1> &d_weight_i, 
  buffer<T, 1> &d_weight_s, 
  buffer<T, 1> &d_weight_j) 
{
  int threads = std::min(e, MAX_KERNEL_THREADS);

  range<1> jw_gws ((e+threads-1)/threads*threads);
  range<1> jw_lws (threads);

  q.submit([&] (handler &cgh) {
    auto csrVal = d_csrVal.template get_access<sycl_read>(cgh);
    auto weight_i = d_weight_i.template get_access<sycl_read>(cgh);
    auto weight_s = d_weight_s.template get_access<sycl_read>(cgh);
    auto weight_j = d_weight_j.template get_access<sycl_discard_write>(cgh);
    cgh.parallel_for<class jw<weighted,T>>(nd_range<1>(jw_gws, jw_lws), [=] (nd_item<1> item) {
      for (int j = item.get_global_id(0); j < e; 
               j += item.get_group_range(0)*item.get_local_range(0)) {
        T Wi =  weight_i[j];
        T Ws =  weight_s[j];
        weight_j[j] = (gamma*csrVal[j])* (Wi/(Ws-Wi));
      }
    });
  });
}

template <bool weighted, typename T>
void fill_weights(queue &q, const int e, buffer<T, 1> &d_w, const T value) 
{
  range<1> fill_gws((e+MAX_KERNEL_THREADS-1)/MAX_KERNEL_THREADS*MAX_KERNEL_THREADS);  
  range<1> fill_lws(MAX_KERNEL_THREADS);
  q.submit([&] (handler &cgh) {
    auto w = d_w.template get_access<sycl_discard_write>(cgh);
    cgh.parallel_for<class fill_elements<weighted, T>>(nd_range<1>(fill_gws, fill_lws), [=] (nd_item<1> item) {
      for (int j = item.get_global_id(0); j<e; j+=item.get_group_range(0)*item.get_local_range(0))
        w[j] = weighted ? (T)(j+1)/e : value; 
    });
  });
}

template <bool weighted, typename T>
void jaccard_weight (queue &q, const int iteration, const int n, const int e, 
    int* csr_ptr, int* csr_ind, T* csr_val)
{

  const T gamma = (T)0.46;  // arbitrary

#ifdef DEBUG
  T* weight_i = (T*) malloc (sizeof(T) * e);
  T* weight_s = (T*) malloc (sizeof(T) * e);
  T* work = (T*) malloc (sizeof(T) * n);
#endif
  T* weight_j = (T*) malloc (sizeof(T) * e);

  {
    buffer<T, 1> d_work (n);
    buffer<T, 1> d_weight_i (e);
    buffer<T, 1> d_weight_s (e);
    buffer<T, 1> d_weight_j (weight_j, e);
    buffer<T, 1> d_csrVal (csr_val, e);
    buffer<int, 1> d_csrPtr (csr_ptr, n+1);
    buffer<int, 1> d_csrInd (csr_ind, e);

    q.wait();
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iteration; i++) {

      fill_weights<weighted, T>(q, e, d_weight_j, (T)1.0);

      // initialize volume of intersections
      fill_weights<false, T>(q, e, d_weight_i, (T)0.0);

      jaccard_row_sum<weighted,T>(q, n, d_csrPtr, d_csrInd, d_weight_j, d_work);

#ifdef DEBUG
      q.submit([&] (handler &cgh) {
        auto work_acc = d_work.template get_access<sycl_read>(cgh);
        cgh.copy(work_acc, work);
      }).wait();
      for (int i = 0; i < n; i++) printf("work: %d %f\n", i, work[i]);
#endif

      // this is the hotspot
      jaccard_is<weighted,T>(q, n, e, d_csrPtr,
          d_csrInd, d_weight_j, d_work, d_weight_i, d_weight_s);

#ifdef DEBUG
      q.submit([&] (handler &cgh) {
        auto weight_i_acc = d_weight_i.template get_access<sycl_read>(cgh);
        cgh.copy(weight_i_acc, weight_i);
      }).wait();
      for (int i = 0; i < e; i++) printf("wi: %d %f\n", i, weight_i[i]);
      q.submit([&] (handler &cgh) {
        auto weight_s_acc = d_weight_s.template get_access<sycl_read>(cgh);
        cgh.copy(weight_s_acc, weight_s);
      }).wait();
      for (int i = 0; i < e; i++) printf("ws: %d %f\n", i, weight_s[i]);
#endif

      // compute jaccard weights
      jaccard_jw<weighted,T>(q, e, d_csrVal, gamma, d_weight_i, d_weight_s, d_weight_j);
    }

    q.wait();
    auto end = std::chrono::steady_clock::now();
    auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "Average execution time of kernels: " << (time * 1e-9f) / iteration << " (s)\n";
  }

#ifdef DEBUG
  // verify using known values when weighted is true
  float error; 

  if (weighted)
    error = std::fabs(weight_j[0] - 0.306667) +
      std::fabs(weight_j[1] - 0.000000) +
      std::fabs(weight_j[2] - 3.680000) +
      std::fabs(weight_j[3] - 1.380000) +
      std::fabs(weight_j[4] - 0.788571) +
      std::fabs(weight_j[5] - 0.460000);

  else
    error = std::fabs(weight_j[0] - 0.230000) +
      std::fabs(weight_j[1] - 0.000000) +
      std::fabs(weight_j[2] - 3.680000) +
      std::fabs(weight_j[3] - 1.380000) +
      std::fabs(weight_j[4] - 0.920000) +
      std::fabs(weight_j[5] - 0.460000);

  if (error > 1e-5) {
    for (int i = 0; i < e; i++) printf("wj: %d %f\n", i, weight_j[i]);
    printf("FAIL");
  } else {
    printf("PASS");
  }
  printf("\n");
#endif

  free(weight_j);
#ifdef DEBUG
  free(weight_i);
  free(weight_s);
  free(work);
#endif
}

// Utilities
void printMatrix(const matrix& M) 
{ 
  int m = M.size(); 
  int n = M[0].size(); 
  for (int i = 0; i < m; i++) { 
    for (int j = 0; j < n; j++) 
      printf("%lf ", M[i][j]);
    printf("\n");
  } 
} 

  template <typename T>
void printVector(const vector<T>& V, char* msg) 
{ 
  printf("%s [ ", msg);
  for_each(V.begin(), V.end(), [](int a) { printf("%d ", a); });
  printf("]\n");
} 

// Reference: https://www.geeksforgeeks.org/sparse-matrix-representations-set-3-csr/
int main(int argc, char** argv) 
{ 
  int iteration = 10;

#ifdef DEBUG
  matrix M  = { 
    { 0, 0, 0, 1}, 
    { 5, 8, 0, 0}, 
    { 0, 0, 3, 0}, 
    { 0, 6, 0, 1} 
  }; 
#else

  int numRow = atoi(argv[1]);
  int numCol = atoi(argv[2]);
  iteration = atoi(argv[3]);

  srand(2);

  matrix M;
  vector<vtype> rowElems(numCol);
  for (int r = 0; r < numRow; r++) {
    for (int c = 0; c < numCol; c++)
      rowElems[c] = rand() % 10;
    M.push_back(rowElems);
  }
#endif

  int row = M.size();
  int col = M[0].size();
  printf("Number of matrix rows and cols: %d %d\n", row, col);
  vector<vtype> csr_val;
  vector<int> csr_ptr = { 0 }; // require -std=c++11  
  vector<int> csr_ind;
  int nnz = 0; // count Number of non-zero elements in each row

  for (int i = 0; i < row; i++) { 
    for (int j = 0; j < col; j++) { 
      if (M[i][j] != (vtype)0) { 
        csr_val.push_back(M[i][j]); 
        csr_ind.push_back(j); 
        nnz++; 
      } 
    } 
    csr_ptr.push_back(nnz); 
  } 

  // print when the matrix is small
  if (row <= 16 && col <= 16) {
    printMatrix(M); 
    printVector(csr_val, (char*)"values = "); 
    printVector(csr_ptr, (char*)"row pointer = "); 
    printVector(csr_ind, (char*)"col indices = "); 
  }

#ifdef USE_GPU
  gpu_selector dev_sel;
#else
  cpu_selector dev_sel;
#endif
  queue q(dev_sel);

  jaccard_weight<true, vtype>(q, iteration, row, nnz, csr_ptr.data(), csr_ind.data(), csr_val.data());
  jaccard_weight<false, vtype>(q, iteration, row, nnz, csr_ptr.data(), csr_ind.data(), csr_val.data());

  return 0; 
} 
