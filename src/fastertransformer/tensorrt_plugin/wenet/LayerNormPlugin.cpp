/*
 * Copyright (c) 2021, NVIDIA CORPORATION.  All rights reserved.
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

#include "LayerNormPlugin.h"
#include "NvInfer.h"

#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>

using namespace nvinfer1;
namespace fastertransformer {

// MHA plugin specific constants
namespace
{
const char* LAYER_NORM_PLUGIN_VERSION{"1"};
const char* LAYER_NORM_PLUGIN_NAME{"LayerNormPlugin"};
} // namespace

// Static class fields initialization
PluginFieldCollection LayerNormPluginCreator::mFC{};
std::vector<PluginField> LayerNormPluginCreator::mPluginAttributes;

REGISTER_TENSORRT_PLUGIN(LayerNormPluginCreator);


// Helper function for serializing plugin
template <typename T>
void writeToBuffer(char*& buffer, const T& val)
{
    *reinterpret_cast<T*>(buffer) = val;
    buffer += sizeof(T);
}

// Helper function for deserializing plugin
template <typename T>
T readFromBuffer(const char*& buffer)
{
    T val = *reinterpret_cast<const T*>(buffer);
    buffer += sizeof(T);
    return val;
}

LayerNormPlugin::LayerNormPlugin(const std::string name)
    : mLayerName(name)
{
    // printf("%s \n", __FUNCTION__);
}

LayerNormPlugin::LayerNormPlugin(const std::string name, const void* data, size_t length)
    : mLayerName(name)
{
    // printf("%s \n", __FUNCTION__);
    // Deserialize in the same order as serialization
    const char* d = static_cast<const char*>(data);
    const char* a = d;

    // isCrossAtten = readFromBuffer<bool>(d);
    // mMHAMax = readFromBuffer<float>(d);
    // printf("LayerNormPlugin::LayerNormPlugin, readFromBuffer isCrossAtten: %s \n", isCrossAtten?"true":"false");

    assert(d == (a + length));
}

const char* LayerNormPlugin::getPluginType() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return LAYER_NORM_PLUGIN_NAME;
}

const char* LayerNormPlugin::getPluginVersion() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return LAYER_NORM_PLUGIN_VERSION;
}

int LayerNormPlugin::getNbOutputs() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return 1;
}

DimsExprs LayerNormPlugin::getOutputDimensions(int32_t        outputIndex,
                                    const DimsExprs *   inputs,
                                    int32_t             nbInputs,
                                    IExprBuilder &      exprBuilder) noexcept
{
    // printf("%s \n", __FUNCTION__);
    // Validate input arguments
    assert(nbInputs == 3);
    assert(outputIndex == 0);

    // MHAping doesn't change input dimension, so output Dims will be the same as input Dims
    return inputs[0];
}

int LayerNormPlugin::initialize() noexcept
{

    return 0;
}

void LayerNormPlugin::terminate() noexcept
{
    // if(isCrossAtten)
    //     delete cross_attn;
    // printf("%s \n", __FUNCTION__);
    // delete cublas_wrapper;
    // printf("%s \n", __FUNCTION__);
    // // allocator = nullptr;
    // delete cublasAlgoMap_;
    // delete cublasWrapperMutex_;
    // printf("%s \n", __FUNCTION__);
    // delete allocator;
    // printf("%s \n", __FUNCTION__);
    // printf("%s \n", __FUNCTION__);

}

size_t LayerNormPlugin::getWorkspaceSize(const PluginTensorDesc *    inputDesc,
                            int32_t                     nbInputs,
                            const PluginTensorDesc *    outputs,
                            int32_t                     nbOutputs 
                            )   const noexcept
{
    size_t workspaceSize = 0;
    // printf("%s \n", __FUNCTION__);
    // int batch_size  = inputDesc[0].dims.d[0];    // B
    // int seq_len0    = inputDesc[0].dims.d[1];    // T0 for q
    // int d_model     = inputDesc[0].dims.d[2];    // D
    // int seq_len1    = inputDesc[1].dims.d[1];    // T1 for k v 
    
    // int dataNum = d_model*(seq_len0*4 + seq_len1*4 +seq_len0*seq_len1*8);    //  q_buf, out_buf, k_buf, v_buf
    // workspaceSize += batch_size*dataNum*sizeof(float);
    return workspaceSize;
}
void LayerNormPlugin::attachToContext(cudnnContext * cudnn_handle,
                        cublasContext * cublas_handle,
                        IGpuAllocator * gpu_allocator
                        )noexcept
{
    // printf("%s \n", __FUNCTION__);
    cublasHandle_ = cublas_handle;
    gpu_allocator_ = gpu_allocator;
}

template<typename T>
void dump2Txt(T* src, int N, std::string fname)
{  
    FILE * pFile;
    pFile = fopen(fname.c_str(),"w");

    T dst_cpu[N];
    cudaMemcpy(dst_cpu, src, N*sizeof(T), cudaMemcpyDeviceToHost);
    for(int i=0; i<N; i++)
    {
        fprintf(pFile, "%f\n", (float)(dst_cpu[i]));
    }
    fclose(pFile);
}

int LayerNormPlugin::enqueue(const PluginTensorDesc*  inputDesc,
                    const PluginTensorDesc* outputDesc,
                    const void *const *     inputs,
                    void *const *           outputs,
                    void *                  workspace,
                    cudaStream_t            stream) noexcept
{
    // cudaStreamSynchronize(stream);
    //  input : q, enc_in, enc_lens, qw, qb, kw, kb, vw, vb, lw, lb
    int status = 0;
    const int batch_size  = inputDesc[0].dims.d[0];    // B
    const int seq_len0    = inputDesc[0].dims.d[1];    // T0 for q
    const int d_model     = inputDesc[0].dims.d[2];    // D
    int widx = 0;
    const void *data_in               = inputs[widx++];
    const void *layer_norm_gamma      = inputs[widx++];
    const void *layer_norm_beta       = inputs[widx++];
    
    // dump2Txt((float*)(query_in), batch_size*seq_len0*d_model, "dump_trt_input/query_in.txt");
    // dump2Txt((float*)(enc_in), batch_size*seq_len1*d_model, "dump_trt_input/enc_in.txt");
    // dump2Txt((int*)(enc_mask), batch_size/10, "dump_trt_input/enc_mask.txt");
    // dump2Txt((float*)(query_weight_kernel), d_model*d_model, "dump_trt_input/query_kernel.txt");
    // dump2Txt((float*)(query_weight_bias)  , d_model,         "dump_trt_input/query_bias.txt");
    // dump2Txt((float*)(key_weight_kernel), d_model*d_model, "dump_trt_input/key_kernel.txt");
    // dump2Txt((float*)(key_weight_bias)  , d_model,         "dump_trt_input/key_bias.txt");
    // dump2Txt((float*)(value_weight_kernel), d_model*d_model, "dump_trt_input/value_kernel.txt");
    // dump2Txt((float*)(value_weight_bias)  , d_model,         "dump_trt_input/value_bias.txt");
    // dump2Txt((float*)(output_weight_kernel), d_model*d_model, "dump_trt_input/linear_kernel.txt");
    // dump2Txt((float*)(output_weight_bias)  , d_model,         "dump_trt_input/linear_bias.txt");
    // printf("debug  inputs %d %d %d, %d %d %d \n", batch_size, seq_len0, d_model, 
    //                 inputDesc[1].dims.d[0], inputDesc[1].dims.d[1], inputDesc[1].dims.d[2]);
    // cudaStreamSynchronize(stream);
    // cublasSetStream(cublasHandle_, stream);
    if(inputDesc[0].type == DataType::kFLOAT)
    {
        invokeGeneralLayerNorm((float*)(outputs[0]),     //  float* out,
                        (float*)data_in,        //  const float* input,
                        (float*)layer_norm_gamma,   //  const float* gamma,
                        (float*)layer_norm_beta,    //  const float* beta,
                        batch_size*seq_len0,    //  const int m,
                        d_model,                //  const int n,
                        stream,             //  cudaStream_t stream,
                        0                   //  int opt_version
                    );
    }
    else if(inputDesc[0].type == DataType::kHALF)
    {
        invokeGeneralLayerNorm((half*)(outputs[0]),     //  float* out,
                        (half*)data_in,        //  const float* input,
                        (half*)layer_norm_gamma,   //  const float* gamma,
                        (half*)layer_norm_beta,    //  const float* beta,
                        batch_size*seq_len0,    //  const int m,
                        d_model,                //  const int n,
                        stream,             //  cudaStream_t stream,
                        0                   //  int opt_version
                    );
    }

    return status;
}

size_t LayerNormPlugin::getSerializationSize() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    size_t ssize = 0;
    // ssize += sizeof(bool);
    return ssize;
}

void LayerNormPlugin::serialize(void* buffer) const noexcept
{
    // printf("%s \n", __FUNCTION__);
    char* d = static_cast<char*>(buffer);
    const char* a = d;

    // writeToBuffer(d, isCrossAtten);
    // // writeToBuffer(d, mMHAMax);

    assert(d == a + getSerializationSize());
}

bool LayerNormPlugin::supportsFormatCombination(int32_t               pos,
                                        const PluginTensorDesc *inOut,
                                        int32_t                 nbInputs,
                                        int32_t                 nbOutputs 
                                        ) noexcept
{
    // std::cout <<"pos " << pos << " format " << (int)inOut[pos].format 
    //     << " type " << (int)inOut[pos].type << std::endl;
    // printf("%s \n", __FUNCTION__);
    // switch (pos)
    // {
    // case 2:
    //     return inOut[pos].type == DataType::kINT32;
    //     break;
    
    // default:
    //     break;
    // }
    return inOut[pos].format  == TensorFormat::kLINEAR && 
        inOut[pos].type == DataType::kFLOAT; // || 
            // inOut[pos].type == DataType::kHALF;
}


void LayerNormPlugin::destroy() noexcept
{
    // This gets called when the network containing plugin is destroyed
    // printf("%s \n", __FUNCTION__);
    delete this;
}

IPluginV2DynamicExt* LayerNormPlugin::clone() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    auto plugin = new LayerNormPlugin(mLayerName);
    plugin->setPluginNamespace(mNamespace.c_str());
    // printf("clone cublasHandle_ %p \n ", plugin->cublasHandle_);
    return plugin;
}

void LayerNormPlugin::setPluginNamespace(const char* libNamespace) noexcept
{
    // printf("%s \n", __FUNCTION__);
    mNamespace = libNamespace;
}

const char* LayerNormPlugin::getPluginNamespace() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return mNamespace.c_str();
}

LayerNormPluginCreator::LayerNormPluginCreator()
{
    // Describe LayerNormPlugin's required PluginField arguments
    // mPluginAttributes.emplace_back(PluginField("MHAMin", nullptr, PluginFieldType::kFLOAT32, 1));
    // printf("%s \n", __FUNCTION__);
    // mPluginAttributes.emplace_back(PluginField("AttentionType", nullptr, PluginFieldType::kCHAR, 4));

    // Fill PluginFieldCollection with PluginField arguments metadata
    mFC.nbFields = mPluginAttributes.size();
    mFC.fields = mPluginAttributes.data();
}

const char* LayerNormPluginCreator::getPluginName() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return LAYER_NORM_PLUGIN_NAME;
}

const char* LayerNormPluginCreator::getPluginVersion() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return LAYER_NORM_PLUGIN_VERSION;
}

const PluginFieldCollection* LayerNormPluginCreator::getFieldNames() noexcept
{
    return &mFC;
}

IPluginV2DynamicExt* LayerNormPluginCreator::createPlugin(const char* name, const PluginFieldCollection* fc) noexcept
{
    const PluginField* fields = fc->fields;
    // printf("createPlugin LayerNormPlugin, nbFields: %d \n", fc->nbFields);

    // Parse fields from PluginFieldCollection
    // assert(fc->nbFields == 2);
    bool isCrossAtten = false;
    for (int i = 0; i < fc->nbFields; i++)
    {
        // std::cout << fields[i].name <<std::endl;
        // if (strcmp(fields[i].name, "AttentionType") == 0)
        // {
        //     assert(fields[i].type == PluginFieldType::kCHAR);
            //  0: self attention, 1: cross attention
            // if(strcmp(reinterpret_cast<const char*>(fields[i].data), "cross") == 0)
            //     isCrossAtten = true;
            // printf("createPlugin AttentionType : %s , %d\n", 
            //     reinterpret_cast<const char*>(fields[i].data), 
            //     isCrossAtten);
        // }
    }
    return new LayerNormPlugin(name);
}

IPluginV2DynamicExt* LayerNormPluginCreator::deserializePlugin(const char* name, const void* serialData, size_t serialLength) noexcept
{
    // This object will be deleted when the network is destroyed, which will
    // call LayerNormPlugin::destroy()
    return new LayerNormPlugin(name, serialData, serialLength);
}

void LayerNormPluginCreator::setPluginNamespace(const char* libNamespace) noexcept
{
    mNamespace = libNamespace;
}

const char* LayerNormPluginCreator::getPluginNamespace() const noexcept
{
    return mNamespace.c_str();
}
}
