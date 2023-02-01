/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "oneflow/core/ep/cuda/cuda_stream.h"
#include "oneflow/core/job/global_for.h"
#include "oneflow/core/job/resource_desc.h"
#include "oneflow/core/hardware/node_device_descriptor_manager.h"
#include "oneflow/core/hardware/cuda_device_descriptor.h"
#include "oneflow/core/ep/cuda/cuda_event.h"
#include "oneflow/core/ep/cuda/cuda_device.h"

#ifdef WITH_CUDA

namespace oneflow {

namespace ep {

namespace {

constexpr size_t kDefaultWorkspaceSizeMb = 4;  // 4M

void SetAffinityByDevice(int dev_id) {
  auto node_device_desc_mgr = Singleton<hardware::NodeDeviceDescriptorManager>::Get();
  if (node_device_desc_mgr == nullptr) { return; }
  auto node_device_desc = node_device_desc_mgr->GetLocalNodeDeviceDescriptor();
  auto cuda_device = std::dynamic_pointer_cast<const hardware::CudaDeviceDescriptor>(
      node_device_desc->GetDevice(hardware::kCudaDeviceDescriptorClassName, dev_id));
  if (!cuda_device) { return; }
  node_device_desc->Topology()->SetCPUAffinityByPCIBusID(cuda_device->PCIBusID());
  node_device_desc->Topology()->SetMemoryAffinityByPCIBusID(cuda_device->PCIBusID());
}

void CheckVersionCompatibility(int compiletime_major, int compiletime_minor, int runtime_major,
                               int runtime_minor, const std::string& name) {
  if (runtime_major != compiletime_major || runtime_minor < compiletime_minor) {
    LOG(WARNING) << "Runtime version " << runtime_major << "." << runtime_minor << " of " << name
                 << " incompatible with compiletime version " << compiletime_major << "."
                 << compiletime_minor << ".";
  }
}

void CheckCudaRuntimeVersion() {
#if !defined(CUDART_VERSION)
#error
#endif  // !defined(CUDART_VERSION)
  const int compiletime_major = CUDART_VERSION / 1000;
  const int compiletime_minor = CUDART_VERSION % 1000 / 10;
  int runtime_version = 0;
  OF_CUDA_CHECK(cudaRuntimeGetVersion(&runtime_version));
  const int runtime_major = runtime_version / 1000;
  const int runtime_minor = runtime_version % 1000 / 10;
  CheckVersionCompatibility(compiletime_major, compiletime_minor, runtime_major, runtime_minor,
                            "CUDA Runtime");
}

void CheckCublasVersion(cublasHandle_t handle) {
#if CUDA_VERSION >= 10020
#if (!defined(CUBLAS_VER_MAJOR)) || (!defined(CUBLAS_VER_MINOR))
#error
#endif  // (!defined(CUBLAS_VER_MAJOR)) || (!defined(CUBLAS_VER_MINOR))
  int runtime_version = 0;
  OF_CUBLAS_CHECK(cublasGetVersion(handle, &runtime_version));
  int runtime_major = 0;
  int runtime_minor = 0;
  if (runtime_version >= 100000) {
    runtime_major = runtime_version / 10000;
    runtime_minor = runtime_version % 10000 / 100;
  } else {
    runtime_major = runtime_version / 1000;
    runtime_minor = runtime_version % 1000 / 100;
  }
  CheckVersionCompatibility(CUBLAS_VER_MAJOR, CUBLAS_VER_MINOR, runtime_major, runtime_minor,
                            "cuBLAS");
#endif  // CUDA_VERSION >= 10020
}

void CheckCudnnVersion() {
#if (!defined(CUDNN_MAJOR)) || (!defined(CUDNN_MINOR))
#error
#endif  // (!defined(CUDNN_MAJOR)) || (!defined(CUDNN_MINOR))
  int runtime_major = 0;
  int runtime_minor = 0;
  OF_CUDNN_CHECK(cudnnGetProperty(libraryPropertyType::MAJOR_VERSION, &runtime_major));
  OF_CUDNN_CHECK(cudnnGetProperty(libraryPropertyType::MINOR_VERSION, &runtime_minor));
  CheckVersionCompatibility(CUDNN_MAJOR, CUDNN_MINOR, runtime_major, runtime_minor, "cuDNN");
}

}  // namespace

#ifdef WITH_CUDA_GRAPHS

CudaGraphExecutable::CudaGraphExecutable() : graph_exec_(nullptr), dev_(-1) {}

CudaGraphExecutable::~CudaGraphExecutable() { Reset(); }

void CudaGraphExecutable::Update(cudaGraph_t graph) {
  int dev = -1;
  OF_CUDA_CHECK(cudaGetDevice(&dev));
  if (dev != dev_) { Reset(); }
  dev_ = dev;
  if (graph_exec_ != nullptr) {
#if CUDA_VERSION < 12000
    cudaGraphExecUpdateResult update_result{};
    cudaGraphNode_t error_node = nullptr;
    OF_CUDA_CHECK(cudaGraphExecUpdate(graph_exec_, graph, &error_node, &update_result));
    if (update_result == cudaGraphExecUpdateSuccess) { return; }
#else
    cudaGraphExecUpdateResultInfo update_result{};
    OF_CUDA_CHECK(cudaGraphExecUpdate(graph_exec_, graph, &update_result));
    if (update_result.result == cudaGraphExecUpdateSuccess) { return; }
#endif  // CUDA_VERSION < 12000
  }
  Reset();
  OF_CUDA_CHECK(cudaGraphInstantiate(&graph_exec_, graph, NULL, NULL, 0));
}

void CudaGraphExecutable::Launch(cudaStream_t stream) const {
  OF_CUDA_CHECK(cudaGraphLaunch(graph_exec_, stream));
}

bool CudaGraphExecutable::IsInstantiated() const { return graph_exec_ != nullptr; }

void CudaGraphExecutable::Reset() {
  if (graph_exec_ != nullptr) {
    CudaCurrentDeviceGuard guard(dev_);
    OF_CUDA_CHECK(cudaGraphExecDestroy(graph_exec_));
  }
}

#endif  // WITH_CUDA_GRAPHS

CudaStream::CudaStream(CudaDevice* device)
    : device_index_(device->device_index()), device_(device) {
  CudaCurrentDeviceGuard guard(device_index_);

  const bool need_check_version = []() {
    static std::atomic<bool> version_checked(false);
    return version_checked.exchange(true) == false;
  }();

  if (need_check_version) { CheckCudaRuntimeVersion(); }

  // cuda_stream
  const char* stream_flags_env_name = "ONEFLOW_EP_CUDA_STREAM_FLAGS";
  if (std::getenv(stream_flags_env_name) != nullptr) {
    const unsigned int stream_flags = ParseIntegerFromEnv(stream_flags_env_name, 0);
    OF_CUDA_CHECK(cudaStreamCreateWithFlags(&cuda_stream_, stream_flags));
  } else {
    OF_CUDA_CHECK(cudaStreamCreate(&cuda_stream_));
  }
  // cublas_handle
  OF_CUBLAS_CHECK(cublasCreate(&cublas_handle_));
  OF_CUBLAS_CHECK(cublasSetStream(cublas_handle_, cuda_stream_));
  if (need_check_version) { CheckCublasVersion(cublas_handle_); }
#if CUDA_VERSION >= 10010
  // cublas_lt_handle
  OF_CUBLAS_CHECK(cublasLtCreate(&cublas_lt_handle_));
#endif
#if CUBLAS_VERSION >= 11000
  if (ParseBooleanFromEnv("ONEFLOW_EP_CUDA_ENABLE_TF32_EXECUTION", true)) {
    OF_CUBLAS_CHECK(cublasSetMathMode(cublas_handle_, CUBLAS_TF32_TENSOR_OP_MATH));
  }
#endif  // CUBLAS_VERSION >= 11000
  // cusolver_dn_handle
#if CUDA_VERSION >= 11000
  OF_CUSOLVER_CHECK(cusolverDnCreate(&cusolver_dn_handle_));
  OF_CUSOLVER_CHECK(cusolverDnSetStream(cusolver_dn_handle_, cuda_stream_));
#endif
  workspace_size_ =
      ParseIntegerFromEnv("ONEFLOW_EP_CUDA_CUBLAS_WORKSPACE_SIZE_MB", kDefaultWorkspaceSizeMb)
      * 1024 * 1024;
  OF_CUDA_CHECK(cudaMalloc(&workspace_, workspace_size_));
#if CUBLAS_VERSION >= 11200
  OF_CUBLAS_CHECK(cublasSetWorkspace(cublas_handle_, workspace_, workspace_size_));
#endif  // CUBLAS_VERSION >= 11200
  // cudnn_handle
  OF_CUDNN_CHECK(cudnnCreate(&cudnn_handle_));
  OF_CUDNN_CHECK(cudnnSetStream(cudnn_handle_, cuda_stream_));
  if (need_check_version) { CheckCudnnVersion(); }
}

CudaStream::~CudaStream() {
  CudaCurrentDeviceGuard guard(device_index_);
  OF_CUDA_CHECK(cudaStreamSynchronize(cuda_stream_));
  OF_CUDNN_CHECK(cudnnDestroy(cudnn_handle_));
  OF_CUBLAS_CHECK(cublasDestroy(cublas_handle_));
#if CUDA_VERSION >= 11000
  OF_CUSOLVER_CHECK(cusolverDnDestroy(cusolver_dn_handle_));
#endif
#if CUDA_VERSION >= 10010
  OF_CUBLAS_CHECK(cublasLtDestroy(cublas_lt_handle_));
#endif
  OF_CUDA_CHECK(cudaStreamDestroy(cuda_stream_));
  OF_CUDA_CHECK(cudaFree(workspace_));
}

Maybe<void> CudaStream::OnExecutionContextSetup() {
  OF_CUDA_CHECK(cudaSetDevice(device_index_));
  SetAffinityByDevice(device_index_);
  return Maybe<void>::Ok();
}

Maybe<void> CudaStream::OnExecutionContextTeardown() { return Maybe<void>::Ok(); }

DeviceType CudaStream::device_type() const { return DeviceType::kCUDA; }

CudaDevice* CudaStream::device() const { return device_; }

Maybe<void> CudaStream::Sync() {
  cudaError_t err = cudaStreamSynchronize(cuda_stream_);
  if (err == cudaSuccess) {
    return Maybe<void>::Ok();
  } else {
    return Error::RuntimeError() << cudaGetErrorString(err) << " (" << err << ") ";
  }
}

void CudaStream::RecordEvent(Event* event) {
  auto* cuda_event = static_cast<CudaEvent*>(event);  // NOLINT
  OF_CUDA_CHECK(cudaEventRecord(cuda_event->cuda_event(), cuda_stream_));
}

Maybe<void> CudaStream::GetAsyncError() {
  cudaError_t err = cudaGetLastError();
  if (err == cudaSuccess) {
    return Maybe<void>::Ok();
  } else {
    return Error::RuntimeError() << cudaGetErrorString(err) << " (" << err << ") ";
  }
}

cudaStream_t CudaStream::cuda_stream() const { return cuda_stream_; }

cublasHandle_t CudaStream::cublas_handle() const { return cublas_handle_; }

#if CUDA_VERSION >= 11000
cusolverDnHandle_t CudaStream::cusolver_dn_handle() const { return cusolver_dn_handle_; }
#endif

#if CUDA_VERSION >= 10010
cublasLtHandle_t CudaStream::cublas_lt_handle() const { return cublas_lt_handle_; }
#endif

void* CudaStream::cublas_workspace() const { return workspace_; }

size_t CudaStream::cublas_workspace_size() const { return workspace_size_; }

cudnnHandle_t CudaStream::cudnn_handle() const { return cudnn_handle_; }

const cudaDeviceProp& CudaStream::device_properties() const { return device_->properties(); }

int CudaStream::cuda_arch() const {
  return device_->properties().major * 100 + device_->properties().minor * 10;
}

#ifdef WITH_CUDA_GRAPHS

void CudaStream::BeginGraphCapture() {
  CHECK(!is_graph_capturing_);
  is_graph_capturing_ = true;
  OF_CUDA_CHECK(cudaStreamBeginCapture(cuda_stream_, cudaStreamCaptureModeThreadLocal));
}

void CudaStream::EndGraphCapture(CudaGraphExecutable* executable) {
  cudaGraph_t graph = nullptr;
  OF_CUDA_CHECK(cudaStreamEndCapture(cuda_stream_, &graph));
  executable->Update(graph);
  OF_CUDA_CHECK(cudaGraphDestroy(graph));
  is_graph_capturing_ = false;
}

bool CudaStream::IsGraphCapturing() const { return is_graph_capturing_; }

void CudaStream::LaunchGraph(const CudaGraphExecutable* executable) {
  executable->Launch(cuda_stream_);
}

#endif  // WITH_CUDA_GRAPHS

}  // namespace ep

}  // namespace oneflow

#endif  // WITH_CUDA

#ifdef WITH_ROCM

namespace oneflow {

namespace ep {

namespace {

constexpr size_t kDefaultWorkspaceSize = 4 * 1024 * 1024;  // 4M

void SetAffinityByDevice(int dev_id) {
  auto node_device_desc_mgr = Singleton<hardware::NodeDeviceDescriptorManager>::Get();
  if (node_device_desc_mgr == nullptr) { return; }
  auto node_device_desc = node_device_desc_mgr->GetLocalNodeDeviceDescriptor();
  auto cuda_device = std::dynamic_pointer_cast<const hardware::CudaDeviceDescriptor>(
      node_device_desc->GetDevice(hardware::kCudaDeviceDescriptorClassName, dev_id));
  if (!cuda_device) { return; }
  node_device_desc->Topology()->SetCPUAffinityByPCIBusID(cuda_device->PCIBusID());
  node_device_desc->Topology()->SetMemoryAffinityByPCIBusID(cuda_device->PCIBusID());
}

}  // namespace

#ifdef WITH_ROCM_GRAPHS

CudaGraphExecutable::CudaGraphExecutable() : graph_exec_(nullptr), dev_(-1) {}

CudaGraphExecutable::~CudaGraphExecutable() { Reset(); }

void CudaGraphExecutable::Update(hipGraph_t graph) {
  int dev = -1;
  OF_CUDA_CHECK(hipGetDevice(&dev));
  if (dev != dev_) { Reset(); }
  dev_ = dev;
  if (graph_exec_ != nullptr) {
    hipGraphExecUpdateResult update_result{};
    hipGraphNode_t error_node = nullptr;
    OF_CUDA_CHECK(hipGraphExecUpdate(graph_exec_, graph, &error_node, &update_result));
    if (update_result == hipGraphExecUpdateSuccess) { return; }
  }
  Reset();
  OF_CUDA_CHECK(hipGraphInstantiate(&graph_exec_, graph, NULL, NULL, 0));
}

void CudaGraphExecutable::Launch(hipStream_t stream) const {
  OF_CUDA_CHECK(hipGraphLaunch(graph_exec_, stream));
}

bool CudaGraphExecutable::IsInstantiated() const { return graph_exec_ != nullptr; }

void CudaGraphExecutable::Reset() {
  if (graph_exec_ != nullptr) {
    CudaCurrentDeviceGuard guard(dev_);
    OF_CUDA_CHECK(hipGraphExecDestroy(graph_exec_));
  }
}

#endif  // WITH_ROCM_GRAPHS

CudaStream::CudaStream(CudaDevice* device)
    : device_index_(device->device_index()), device_(device) {
  CudaCurrentDeviceGuard guard(device_index_);
  // cuda_stream
  const char* stream_flags_env_name = "ONEFLOW_EP_CUDA_STREAM_FLAGS";
  if (std::getenv(stream_flags_env_name) != nullptr) {
    const unsigned int stream_flags = ParseIntegerFromEnv(stream_flags_env_name, 0);
    OF_CUDA_CHECK(hipStreamCreateWithFlags(&cuda_stream_, stream_flags));
  } else {
    OF_CUDA_CHECK(hipStreamCreate(&cuda_stream_));
  }  // cublas_handle
  OF_CUBLAS_CHECK(hipblasCreate(&cublas_handle_));
  OF_CUBLAS_CHECK(hipblasSetStream(cublas_handle_, cuda_stream_));

  OF_CUSOLVER_CHECK(hipsolverDnCreate(&cusolver_dn_handle_));
  OF_CUSOLVER_CHECK(hipsolverDnSetStream(cusolver_dn_handle_, cuda_stream_));

  workspace_size_ = kDefaultWorkspaceSize;
  OF_CUDA_CHECK(hipMalloc(&workspace_, workspace_size_));

  OF_CUDNN_CHECK(hipdnnCreate(&cudnn_handle_));
 
  OF_CUDNN_CHECK(hipdnnSetStream(cudnn_handle_, cuda_stream_));
}

CudaStream::~CudaStream() {
  CudaCurrentDeviceGuard guard(device_index_);
  OF_CUDA_CHECK(hipStreamSynchronize(cuda_stream_));
  OF_CUDNN_CHECK(hipdnnDestroy(cudnn_handle_));
  OF_CUBLAS_CHECK(hipblasDestroy(cublas_handle_));
  OF_CUSOLVER_CHECK(hipsolverDnDestroy(cusolver_dn_handle_));
  OF_CUDA_CHECK(hipStreamDestroy(cuda_stream_));
  OF_CUDA_CHECK(hipFree(workspace_));
}

Maybe<void> CudaStream::OnExecutionContextSetup() {
  OF_CUDA_CHECK(hipSetDevice(device_index_));
  SetAffinityByDevice(device_index_);
  return Maybe<void>::Ok();
}

Maybe<void> CudaStream::OnExecutionContextTeardown() { return Maybe<void>::Ok(); }

DeviceType CudaStream::device_type() const { return DeviceType::kCUDA; }

CudaDevice* CudaStream::device() const { return device_; }

Maybe<void> CudaStream::Sync() {
  hipError_t err = hipStreamSynchronize(cuda_stream_);
  if (err == hipSuccess) {
    return Maybe<void>::Ok();
  } else {
    return Error::RuntimeError() << hipGetErrorString(err) << " (" << err << ") ";
  }
}

void CudaStream::RecordEvent(Event* event) {
  auto* cuda_event = static_cast<CudaEvent*>(event);  // NOLINT
  OF_CUDA_CHECK(hipEventRecord(cuda_event->cuda_event(), cuda_stream_));
}

Maybe<void> CudaStream::GetAsyncError() {
  hipError_t err = hipGetLastError();
  if (err == hipSuccess) {
    return Maybe<void>::Ok();
  } else {
    return Error::RuntimeError() << hipGetErrorString(err) << " (" << err << ") ";
  }
}

hipStream_t CudaStream::cuda_stream() const { return cuda_stream_; }

hipblasHandle_t CudaStream::cublas_handle() const { return cublas_handle_; }

hipsolverHandle_t CudaStream::cusolver_dn_handle() const { return cusolver_dn_handle_; }

void* CudaStream::cublas_workspace() const { return workspace_; }

size_t CudaStream::cublas_workspace_size() const { return workspace_size_; }

hipdnnHandle_t CudaStream::cudnn_handle() const { return cudnn_handle_; }

const hipDeviceProp_t& CudaStream::device_properties() const { return device_->properties(); }

int CudaStream::cuda_arch() const {
  return device_->properties().major * 100 + device_->properties().minor * 10;
}

#ifdef WITH_ROCM_GRAPHS

void CudaStream::BeginGraphCapture() {
  CHECK(!is_graph_capturing_);
  is_graph_capturing_ = true;
  OF_CUDA_CHECK(hipStreamBeginCapture(cuda_stream_, hipStreamCaptureModeThreadLocal));
}

void CudaStream::EndGraphCapture(CudaGraphExecutable* executable) {
  hipGraph_t graph = nullptr;
  OF_CUDA_CHECK(hipStreamEndCapture(cuda_stream_, &graph));
  executable->Update(graph);
  OF_CUDA_CHECK(hipGraphDestroy(graph));
  is_graph_capturing_ = false;
}

bool CudaStream::IsGraphCapturing() const { return is_graph_capturing_; }

void CudaStream::LaunchGraph(const CudaGraphExecutable* executable) {
  executable->Launch(cuda_stream_);
}

#endif  // WITH_ROCM_GRAPHS

}  // namespace ep

}  // namespace oneflow

#endif  // WITH_ROCM
