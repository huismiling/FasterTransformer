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

#include "MHAPlugin.h"
// #include "decoder_masked_multihead_attention.h"
#include "NvInfer.h"
// #include "decoder_masked_multihead_attention_utils.h"
// #include "bfloat16_fallback_kenrels.cuh"

#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>

using namespace nvinfer1;
using namespace std;

namespace fastertransformer {

// MHA plugin specific constants
namespace
{
const char* MHA_PLUGIN_VERSION{"1"};
const char* MHA_PLUGIN_NAME{"MultiHeadAttn"};
} // namespace

// Static class fields initialization
PluginFieldCollection MHAPluginCreator::mFC{};
std::vector<PluginField> MHAPluginCreator::mPluginAttributes;

REGISTER_TENSORRT_PLUGIN(MHAPluginCreator);

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

MHAPlugin::MHAPlugin(const std::string name, bool isCrossAtten)
    : mLayerName(name),
    isCrossAtten(isCrossAtten)
{
    // printf("%s \n", __FUNCTION__);
}

MHAPlugin::MHAPlugin(const std::string name, const void* data, size_t length)
    : mLayerName(name)
{
    // printf("%s \n", __FUNCTION__);
    // Deserialize in the same order as serialization
    const char* d = static_cast<const char*>(data);
    const char* a = d;

    isCrossAtten = readFromBuffer<bool>(d);
    // mMHAMax = readFromBuffer<float>(d);
    // printf("MHAPlugin::MHAPlugin, readFromBuffer isCrossAtten: %s \n", isCrossAtten?"true":"false");

    assert(d == (a + length));
}

const char* MHAPlugin::getPluginType() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return MHA_PLUGIN_NAME;
}

const char* MHAPlugin::getPluginVersion() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return MHA_PLUGIN_VERSION;
}

int MHAPlugin::getNbOutputs() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return 1;
}

DimsExprs MHAPlugin::getOutputDimensions(int32_t        outputIndex,
                                    const DimsExprs *   inputs,
                                    int32_t             nbInputs,
                                    IExprBuilder &      exprBuilder) noexcept
{
    // printf("%s \n", __FUNCTION__);
    // Validate input arguments
    assert(nbInputs == 13);
    assert(outputIndex == 0);

    // MHAping doesn't change input dimension, so output Dims will be the same as input Dims
    return inputs[0];
}

int MHAPlugin::initialize() noexcept
{
    // cublas_algo_map = new ft::cublasAlgoMap("igemm_config.in");
    printf("%s \n", __FUNCTION__);

    sm = getSMVersion();

    allocator = new Allocator<AllocatorType::CUDA>(getDevice());

    cublas_wrapper_mutex = new std::mutex();

    cublasLtCreate(&cublaslt_handle_);

    if(int8_model == -1)
        cublas_wrapper = new cublasMMWrapper(cublas_handle_,
                                    cublaslt_handle_,
                                    nullptr,
                                    cublas_algo_map,
                                    cublas_wrapper_mutex,
                                    nullptr);
    else
        cublas_wrapper = (cublasMMWrapper*)new cublasINT8MMWrapper(cublaslt_handle_,
                                    nullptr,
                                    cublas_algo_map,
                                    cublas_wrapper_mutex,
                                    true);


    return 0;
}

void MHAPlugin::terminate() noexcept
{
    // if(isCrossAtten)
    //     delete cross_attn;
    // printf("%s \n", __FUNCTION__);
    delete cublas_wrapper;
    // printf("%s \n", __FUNCTION__);
    // // allocator = nullptr;
    // delete cublasAlgoMap_;
    // delete cublasWrapperMutex_;
    // printf("%s \n", __FUNCTION__);
    // delete allocator;
    // printf("%s \n", __FUNCTION__);
    // printf("%s \n", __FUNCTION__);
    delete cublas_wrapper_mutex;
    // delete cublas_algo_map;
    delete allocator;
    cublasLtDestroy(cublaslt_handle_);

}

size_t MHAPlugin::getWorkspaceSize(const PluginTensorDesc *    inputDesc,
                            int32_t                     nbInputs,
                            const PluginTensorDesc *    outputs,
                            int32_t                     nbOutputs 
                            )   const noexcept
{
    // printf("%s \n", __FUNCTION__);
    int batch_size  = inputDesc[0].dims.d[0];    // B
    int seq_len0    = inputDesc[0].dims.d[1];    // T0 for q
    int d_model     = inputDesc[0].dims.d[2];    // D
    int seq_len1    = isCrossAtten ? inputDesc[1].dims.d[1] : seq_len0;    // T1 for k v 
    
    int dataNum = d_model*(seq_len0*4 + seq_len1*4 +seq_len0*seq_len1*8);    //  q_buf, out_buf, k_buf, v_buf
    size_t workspaceSize = batch_size*dataNum*sizeof(float);
    return workspaceSize;
}
void MHAPlugin::attachToContext(cudnnContext * cudnn_handle,
                        cublasContext * cublas_handle,
                        IGpuAllocator * gpu_allocator
                        )noexcept
{
    // printf("%s \n", __FUNCTION__);
    // printf("gpu_allocator %p \n", gpu_allocator);
    cublas_handle_ = cublas_handle;
    gpu_allocator_ = gpu_allocator;
}

int MHAPlugin::pre_enqueue(cudaStream_t stream) noexcept
{
    // printf("%s \n", __FUNCTION__);
    // cublasSetStream(cublasltHandle_, stream);

    return 0;
}

template<typename T>
void dump2Txt(T* src, int N, std::string fname)
{
    FILE * pFile;
    pFile = fopen(fname.c_str(),"w");
    
    std::cout << fname ; 
    T *dst_cpu = new T[N];
    cudaMemcpy(dst_cpu, src, N*sizeof(T), cudaMemcpyDeviceToHost);
    for(int i=0; i<N; i++)
    {
        fprintf(pFile, "%f\n", (float)(dst_cpu[i]));
    }
    std::cout<< "       done" << std::endl;
    fclose(pFile);
    delete []dst_cpu;
}

template <typename T>
int mha_forward_int8(const void *const *     inputs     ,
                void *const *           outputs         ,
                void *                  workspace       ,
                int                     batch_size      ,
                int                     q_len           ,
                int                     d_model         ,
                int                     kv_len          ,
                int                     head_num        ,
                int                     size_per_head   ,
                bool                    isCrossAtten    ,
                float *                 scale_list      ,
                cublasMMWrapper *       cublas_wrapper  ,
                cublasHandle_t          cublas_handle   ,
                cudaStream_t            stream,
                const std::string mLayerName)
{
    const int qm = batch_size*q_len;
    const int kvm = batch_size*kv_len;
    const int k = d_model;
    const int n = d_model;
/*
    int widx = 0;
    T *query_in              = (T*)(inputs[widx++]);
    T *enc_in                = (T*)(inputs[widx++]);
    float  *enc_mask              = (float*)(inputs[widx++]);
    int8_t *query_weight_kernel   = (int8_t*)(inputs[widx++]);
    T      *query_weight_bias     = (T*)(inputs[widx++]);
    int8_t *key_weight_kernel     = (int8_t*)(inputs[widx++]);
    T      *key_weight_bias       = (T*)(inputs[widx++]);
    int8_t *value_weight_kernel   = (int8_t*)(inputs[widx++]);
    T      *value_weight_bias     = (T*)(inputs[widx++]);
    int8_t *output_weight_kernel  = (int8_t*)(inputs[widx++]);
    T      *output_weight_bias    = (T*)(inputs[widx++]);

    int ws_offset  = 0;
    int8_t* q_buf   = (int8_t*)workspace + ws_offset; ws_offset += batch_size*q_len*d_model*sizeof(int8_t);
    int8_t* k_buf   = (int8_t*)workspace + ws_offset; ws_offset += batch_size*kv_len*d_model*sizeof(int8_t);
    int8_t* v_buf   = (int8_t*)workspace + ws_offset; ws_offset += batch_size*kv_len*d_model*sizeof(int8_t);
    int8_t* q_share = (int8_t*)workspace + ws_offset; ws_offset += batch_size*q_len*d_model*sizeof(int8_t);
    int8_t* k_share = (int8_t*)workspace + ws_offset; ws_offset += batch_size*kv_len*d_model*sizeof(int8_t);
    int8_t* v_share = (int8_t*)workspace + ws_offset; ws_offset += batch_size*kv_len*d_model*sizeof(int8_t);
    int8_t* qk_buf0 = (int8_t*)workspace + ws_offset; ws_offset += batch_size*head_num*q_len*kv_len*sizeof(int8_t);
    int8_t* qk_buf1 = (int8_t*)workspace + ws_offset; ws_offset += batch_size*head_num*q_len*kv_len*sizeof(int8_t);
    int8_t* out_buf = (int8_t*)workspace + ws_offset; ws_offset += batch_size*q_len*d_model*sizeof(int32_t);

    cublasINT8MMWrapper* cublas_wrapper_ = (cublasINT8MMWrapper*)cublas_wrapper;

    // cudaStreamSynchronize(stream);
    // cublas_wrapper->setStream(stream);

    // printf("debug  inputs %d %d %d, %d %d %d \n", batch_size, seq_len0, d_model, 
    //                 inputDesc[1].dims.d[0], inputDesc[1].dims.d[1], inputDesc[1].dims.d[2]);

    // dump2Txt((float*)(query_in), batch_size*seq_len0*d_model, "dump_trt_input/"+mLayerName+"_query_in.txt");
    // dump2Txt((float*)(q_share), batch_size*seq_len0*d_model, "dump_trt_input/"+mLayerName+"_query_layer_norm.txt");
    // dump2Txt((float*)(enc_in), batch_size*kv_len*d_model, "dump_trt_input/"+mLayerName+"_enc_in.txt");
    // dump2Txt((float*)(enc_mask), batch_size*seq_len0*kv_len, "dump_trt_input/"+mLayerName+"_enc_mask.txt");
    // dump2Txt((float*)(query_weight_kernel), d_model*d_model, "dump_trt_input/"+mLayerName+"_query_kernel.txt");
    // dump2Txt((float*)(query_weight_bias)  , d_model,         "dump_trt_input/"+mLayerName+"_query_bias.txt");
    // dump2Txt((float*)(key_weight_kernel), d_model*d_model, "dump_trt_input/"+mLayerName+"_key_kernel.txt");
    // dump2Txt((float*)(key_weight_bias)  , d_model,         "dump_trt_input/"+mLayerName+"_key_bias.txt");
    // dump2Txt((float*)(value_weight_kernel), d_model*d_model, "dump_trt_input/"+mLayerName+"_value_kernel.txt");
    // dump2Txt((float*)(value_weight_bias)  , d_model,         "dump_trt_input/"+mLayerName+"_value_bias.txt");
    // dump2Txt((float*)(output_weight_kernel), d_model*d_model, "dump_trt_input/"+mLayerName+"_linear_kernel.txt");
    // dump2Txt((float*)(output_weight_bias)  , d_model,         "dump_trt_input/"+mLayerName+"_linear_bias.txt");
    // dump2Txt((float*)(layer_norm_gamma)  , d_model,         "dump_trt_input/"+mLayerName+"_layer_norm_gamma.txt");
    // dump2Txt((float*)(layer_norm_beta)  , d_model,         "dump_trt_input/"+mLayerName+"_layer_norm_beta.txt");
    int dp=0;
    invokeQuantization(q_buf, query_in, batch_size*q_len*d_model, &(scale_list[0]), stream);
    invokeRowMajorToCOL32((int8_t*)q_share, (int8_t*)q_buf, qm, n, stream);

    if(isCrossAtten){
        invokeQuantization(k_buf, enc_in, batch_size*kv_len*d_model, &(scale_list[1]), stream);
        invokeRowMajorToCOL32((int8_t*)k_share, (int8_t*)k_buf, kvm, n, stream);
    }
    else{
        k_share = q_share;
    }

    const float alpha = 1.0, beta = 0.0;
    cublas_wrapper_->Gemm((int8_t*)q_buf,
                            1,
                            qm,
                            n,
                            k,
                            0,
                            0,
                            0,
                            scale_list[0],
                            q_share,
                            (int8_t*)(query_weight_kernel));
    cublas_wrapper_->Gemm((int8_t*)k_buf,
                            1,
                            kvm,
                            n,
                            k,
                            0,
                            0,
                            0,
                            scale_list[1],
                            k_share,
                            (int8_t*)(key_weight_kernel));
    cublas_wrapper_->Gemm((int8_t*)v_buf,
                            1,
                            kvm,
                            n,
                            k,
                            0,
                            0,
                            0,
                            scale_list[2],
                            k_share,
                            (int8_t*)(value_weight_kernel));

    // cudaStreamSynchronize(stream);
    // dump2Txt((float*)(q_share), batch_size*seq_len0*d_model, "dump_trt/"+mLayerName+"_q_in.txt");
    // dump2Txt((float*)(enc_in), batch_size*kv_len*d_model, "dump_trt/"+mLayerName+"_enc_in.txt");
    // dump2Txt(q_buf, batch_size*seq_len0*d_model, "dump_trt/"+mLayerName+"_q_buf.txt");
    // dump2Txt(k_buf, batch_size*kv_len*d_model, "dump_trt/"+mLayerName+"_k_buf.txt");
    // dump2Txt(v_buf, batch_size*kv_len*d_model, "dump_trt/"+mLayerName+"_v_buf.txt");

    // invokeAddQKVBiasTranspose(q_share,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         q_buf,
    //                         query_weight_bias,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         batch_size,
    //                         seq_len0,
    //                         head_num,
    //                         size_per_head,
    //                         stream);
    // invokeAddQKVBiasTranspose(k_bias,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         k_buf,
    //                         key_weight_bias,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         batch_size,
    //                         kv_len,
    //                         head_num,
    //                         size_per_head,
    //                         stream);

    // invokeAddQKVBiasTranspose(v_bias,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         v_buf,
    //                         value_weight_bias,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         (T*)nullptr,
    //                         batch_size,
    //                         kv_len,
    //                         head_num,
    //                         size_per_head,
    //                         stream);

    invokeAddQKBiasTransform((int8_t*)q_share,
                            (int8_t*)k_share,
                            (const int8_t*)q_buf,
                            query_weight_bias,
                            (const int8_t*)k_buf,
                            key_weight_bias,
                            batch_size,
                            q_len,
                            kv_len,
                            head_num,
                            size_per_head,
                            &(scale_list[2]),        // q_input_deQFactor_ptr,
                            &(scale_list[2]),       // k_input_deQFactor_ptr,
                            &(scale_list[2]),        // q_output_scale_ptr,
                            &(scale_list[2]),       // k_output_scale_ptr,
                            cublas_wrapper_->getUseOrderCol322R4R4(),
                            stream);
    invokeAddVBiasTransform((int8_t*)v_share,
                            (int8_t*)v_buf,
                            value_weight_bias,
                            batch_size,
                            kv_len,
                            head_num,
                            size_per_head,
                            &(scale_list[2]),           // input_deQFactor_ptr,
                            &(scale_list[2]),           // out_scale_ptr,
                            cublas_wrapper_->getUseOrderCol322R4R4(),
                            stream);
    sync_check_cuda_error();

    // cudaStreamSynchronize(stream);
    // dump2Txt(q_share, batch_size*seq_len0*d_model, "dump_trt/"+mLayerName+"_q_bias_trans.txt");
    // dump2Txt(k_bias, batch_size*kv_len*d_model, "dump_trt/"+mLayerName+"_k_bias_trans.txt");
    // dump2Txt(v_bias, batch_size*kv_len*d_model, "dump_trt/"+mLayerName+"_v_bias_trans.txt");

    // cublas_wrapper_->stridedBatchedGemm(CUBLAS_OP_T,
    //         CUBLAS_OP_N,
    //         kv_len,               //m,
    //         q_len,               //n,
    //         size_per_head,          //k,
    //         (const void*)k_bias,                     //A,
    //         size_per_head,              //lda,
    //         kv_len*size_per_head,     //strideA,
    //         (const void*)q_share,                    //B,
    //         size_per_head,              //ldb,
    //         q_len*size_per_head,     //strideB,
    //         (void*)qk_buf,                 //C,
    //         kv_len,               //ldc,
    //         q_len*kv_len,      //strideC,
    //         batch_size*head_num,    //batchCount
    //         1.f,                    // alpha
    //         0.f                     // beta
    //         );

    const int kv_len_padded = (kv_len + 31) / 32 * 32;
    cublas_wrapper_->Gemm((int8_t*)qk_buf0,               // int8_t* res,
                        batch_size*head_num,            // int batchCount,
                        q_len,                          // int m,
                        kv_len_padded,                  // int n,
                        size_per_head,                  // int k,
                        size_per_head * q_len,          // int64_t stridea,
                        size_per_head * kv_len_padded,  // int64_t strideb,
                        q_len * kv_len_padded,          // int64_t stridec,
                        scale_list[2],           // const float alpha,
                        q_share,                        // const int8_t* ATransform,
                        k_buf);                         // const int8_t* kernel)

    // cudaStreamSynchronize(stream);
    // dump2Txt(qk_buf, batch_size*head_num*seq_len0*seq_len1, "dump_trt/"+mLayerName+"_qk_buf.txt");
    
    float scalar = 1 / sqrtf(size_per_head * 1.0f);
    // invokeMaskedSoftMax(qk_buf,
    //                     qk_buf,
    //                     enc_mask,
    //                     batch_size,
    //                     seq_len0,
    //                     seq_len1,
    //                     head_num,
    //                     scalar,
    //                     stream
    //     );
    invokeSoftmaxCOL32(qk_buf1,                  // int8_t* output,
                        (int8_t*)qk_buf0,        // const int8_t* input,
                        enc_mask,               // const T* attr_mask,
                        batch_size,             // const int batch_size,
                        head_num,               // const int head_num,
                        q_len,                  // const int seq_len,
                        scalar,                 // const float scalar1a,
                        &(scale_list[2]),    // const float* scalar1b,
                        &(scale_list[2]),     // const float* amax_ptr,
                        stream);                // cudaStream_t stream)

    // cudaStreamSynchronize(stream);
    // dump2Txt(qk_buf, batch_size*head_num*seq_len0*seq_len1, "dump_trt/"+mLayerName+"_softmax.txt");
    cublas_wrapper_->Gemm((int8_t*)q_buf,                   // int8_t* res,
                            batch_size*head_num,                // int batchCount,
                            q_len,                              // int m,
                            size_per_head,                      // int n,
                            kv_len_padded,                      // int k,
                            q_len * kv_len_padded,              // int64_t stridea,
                            size_per_head * kv_len_padded,      // int64_t strideb,
                            size_per_head * q_len,              // int64_t stridec,
                            scale_list[2],          // const float alpha,
                            qk_buf1,                             // const int8_t* ATransform,
                            v_share);                           // const int8_t* kernel)

    // cublas_wrapper_->stridedBatchedGemm(CUBLAS_OP_N,
    //         CUBLAS_OP_N,
    //         size_per_head,          //m,
    //         seq_len0,               //n,
    //         seq_len1,               //k,
    //         (const void*)v_bias,                     //A,
    //         size_per_head,              //lda,
    //         seq_len1*size_per_head,     //strideA,
    //         (const void*)qk_buf,                     //B,
    //         seq_len1,                   //ldb,
    //         seq_len0*seq_len1,          //strideB,
    //         (void*)q_buf,                  //C,
    //         size_per_head,          //ldc,
    //         seq_len0*size_per_head, //strideC,
    //         batch_size*head_num,    //batchCount
    //         1.f,                    // alpha
    //         0.f                     // beta
    //         );
    // cudaStreamSynchronize(stream);
    // dump2Txt(q_buf, batch_size*head_num*seq_len0*size_per_head, "dump_trt/"+mLayerName+"_qkv.txt");

    invokeTransposeCOL32(out_buf,
                        (const int8_t*)q_buf,
                        batch_size,
                        q_len,
                        head_num,
                        size_per_head,
                        scale_list,   // no used !!! const float* bmm2_deQFactor,
                        scale_list,   // no used !!! const float* out_scale_ptr,
                        stream);

    // invokeTransposeQKV(
    //     out_buf, q_buf, batch_size, seq_len0, head_num, size_per_head, stream);
    // cudaStreamSynchronize(stream);
    // dump2Txt(out_buf, batch_size*head_num*seq_len0*size_per_head, "dump_trt/"+mLayerName+"_qkv_trans.txt");
    // dump2Txt(output_weight_kernel, d_model*d_model, "dump_trt/"+mLayerName+"_lw.txt");
    cublas_wrapper_->Gemm((int8_t*)q_buf,
                        1,
                        qm,
                        n,
                        k,
                        0,
                        0,
                        0,
                        scale_list[2],
                        out_buf,
                        (int8_t*)output_weight_kernel);

    // cublas_wrapper_->Gemm(CUBLAS_OP_N,
    //                     CUBLAS_OP_N,
    //                     d_model,
    //                     batch_size*seq_len0,
    //                     d_model,
    //                     output_weight_kernel,
    //                     d_model,
    //                     (const T*)(out_buf),
    //                     d_model,
    //                     outputs[0],
    //                     d_model);
    // cudaStreamSynchronize(stream);
    // dump2Txt((float*)(outputs[0]), batch_size*head_num*seq_len0*size_per_head, "dump_trt/"+mLayerName+"_out_linear.txt");
    // add_bias_kernel<<<gridq, block, 0, stream>>>(
    //     (float*)(outputs[0]), (float*)(outputs[0]), output_weight_bias, 
    //     batch_size, seq_len0, d_model);
    invokeTransposeCOL32ToRow((int8_t*)q_share,
                              (int8_t*)q_buf,
                              batch_size,
                              q_len,
                              head_num,
                              size_per_head,
                              scale_list, // no used !!! const float* bmm2_deQFactor,
                              scale_list, // no used !!! const float* out_scale_ptr,
                              stream);
    invokeAddBiasResidual((T*)(outputs[0]),     //  T* output, 
                        (int8_t*)q_share,                   //  const T* input, 
                        output_weight_bias,         //  const T* bias, 
                        batch_size * q_len,         //  const int m, 
                        d_model,                    //  const int n, 
                        scale_list[2], //const float out_scale_ptr,
                        stream
                );
*/
    // cudaStreamSynchronize(stream);
    // dump2Txt((float*)(outputs[0]), batch_size*head_num*seq_len0*size_per_head, "dump_trt/"+mLayerName+"_out_final.txt");
    return 0;
}


template <typename T>
int mha_forward(const void *const *     inputs          ,
                void *const *           outputs         ,
                void *                  workspace       ,
                int                     batch_size      ,
                int                     seq_len0        ,
                int                     d_model         ,
                int                     seq_len1        ,
                int                     head_num        ,
                int                     size_per_head   ,
                bool                    isCrossAtten    ,
                cublasMMWrapper *       cublas_wrapper  ,
                cublasHandle_t          cublas_handle   ,
                cudaStream_t            stream,
                const std::string mLayerName)
{
    int widx = 0;
    T *query_in              = (T*)(inputs[widx++]);
    T *enc_in                = (T*)(inputs[widx++]);
    T *enc_mask              = (T*)(inputs[widx++]);
    T *query_weight_kernel   = (T*)(inputs[widx++]);
    T *query_weight_bias     = (T*)(inputs[widx++]);
    T *key_weight_kernel     = (T*)(inputs[widx++]);
    T *key_weight_bias       = (T*)(inputs[widx++]);
    T *value_weight_kernel   = (T*)(inputs[widx++]);
    T *value_weight_bias     = (T*)(inputs[widx++]);
    T *output_weight_kernel  = (T*)(inputs[widx++]);
    T *output_weight_bias    = (T*)(inputs[widx++]);
    T *layer_norm_gamma      = (T*)(inputs[widx++]);
    T *layer_norm_beta       = (T*)(inputs[widx++]);

    int ws_offset  = 0;
    T* q_buf   = (T*)workspace + ws_offset; ws_offset += batch_size*seq_len0*d_model;
    T* q_share = (T*)workspace + ws_offset; ws_offset += batch_size*seq_len0*d_model;
    T* qk_buf  = (T*)workspace + ws_offset; ws_offset += batch_size*head_num*seq_len0*seq_len1;
    T* out_buf = (T*)workspace + ws_offset; ws_offset += batch_size*seq_len0*d_model;
    T* k_buf   = (T*)workspace + ws_offset; ws_offset += batch_size*seq_len1*d_model;
    T* k_bias  = (T*)workspace + ws_offset; ws_offset += batch_size*seq_len1*d_model;
    T* v_buf   = (T*)workspace + ws_offset; ws_offset += batch_size*seq_len1*d_model;
    T* v_bias  = (T*)workspace + ws_offset; ws_offset += batch_size*seq_len1*d_model;

    // cudaStreamSynchronize(stream);
    // cublas_wrapper->setStream(stream);

    invokeGeneralLayerNorm(q_share,     //  float* out,
                        query_in,       //  const float* input,
                        layer_norm_gamma,   //  const float* gamma,
                        layer_norm_beta,    //  const float* beta,
                        batch_size*seq_len0,    //  const int m,
                        d_model,                //  const int n,
                        stream,             //  cudaStream_t stream,
                        0                   //  int opt_version
                    );
    if (! isCrossAtten){     //  self attn
        enc_in = q_share;
    }
    // printf("debug  inputs %d %d %d, %d %d %d \n", batch_size, seq_len0, d_model, 
    //                 inputDesc[1].dims.d[0], inputDesc[1].dims.d[1], inputDesc[1].dims.d[2]);

    // dump2Txt((float*)(query_in), batch_size*seq_len0*d_model, "dump_trt_input/"+mLayerName+"_query_in.txt");
    // dump2Txt((float*)(q_share), batch_size*seq_len0*d_model, "dump_trt_input/"+mLayerName+"_query_layer_norm.txt");
    // dump2Txt((float*)(enc_in), batch_size*seq_len1*d_model, "dump_trt_input/"+mLayerName+"_enc_in.txt");
    // dump2Txt((float*)(enc_mask), batch_size*seq_len0*seq_len1, "dump_trt_input/"+mLayerName+"_enc_mask.txt");
    // dump2Txt((float*)(query_weight_kernel), d_model*d_model, "dump_trt_input/"+mLayerName+"_query_kernel.txt");
    // dump2Txt((float*)(query_weight_bias)  , d_model,         "dump_trt_input/"+mLayerName+"_query_bias.txt");
    // dump2Txt((float*)(key_weight_kernel), d_model*d_model, "dump_trt_input/"+mLayerName+"_key_kernel.txt");
    // dump2Txt((float*)(key_weight_bias)  , d_model,         "dump_trt_input/"+mLayerName+"_key_bias.txt");
    // dump2Txt((float*)(value_weight_kernel), d_model*d_model, "dump_trt_input/"+mLayerName+"_value_kernel.txt");
    // dump2Txt((float*)(value_weight_bias)  , d_model,         "dump_trt_input/"+mLayerName+"_value_bias.txt");
    // dump2Txt((float*)(output_weight_kernel), d_model*d_model, "dump_trt_input/"+mLayerName+"_linear_kernel.txt");
    // dump2Txt((float*)(output_weight_bias)  , d_model,         "dump_trt_input/"+mLayerName+"_linear_bias.txt");
    // dump2Txt((float*)(layer_norm_gamma)  , d_model,         "dump_trt_input/"+mLayerName+"_layer_norm_gamma.txt");
    // dump2Txt((float*)(layer_norm_beta)  , d_model,         "dump_trt_input/"+mLayerName+"_layer_norm_beta.txt");
    int dp=0;

    const float alpha = 1.0, beta = 0.0;
    cublas_wrapper->Gemm(CUBLAS_OP_N,
                        CUBLAS_OP_N,
                        d_model,
                        batch_size*seq_len0,
                        d_model,
                        query_weight_kernel,
                        d_model,
                        (const T*)(q_share),
                        d_model,
                        q_buf,
                        d_model);
    
    cublas_wrapper->Gemm(CUBLAS_OP_N,
                        CUBLAS_OP_N,
                        d_model,
                        batch_size*seq_len1,
                        d_model,
                        key_weight_kernel,
                        d_model,
                        (const T*)(enc_in),
                        d_model,
                        k_buf,
                        d_model);

    cublas_wrapper->Gemm(CUBLAS_OP_N,
                        CUBLAS_OP_N,
                        d_model,
                        batch_size*seq_len1,
                        d_model,
                        value_weight_kernel,
                        d_model,
                        (const T*)(enc_in),
                        d_model,
                        v_buf,
                        d_model);
    // cudaStreamSynchronize(stream);
    // dump2Txt((float*)(q_share), batch_size*seq_len0*d_model, "dump_trt/"+mLayerName+"_q_in.txt");
    // dump2Txt((float*)(enc_in), batch_size*seq_len1*d_model, "dump_trt/"+mLayerName+"_enc_in.txt");
    // dump2Txt(q_buf, batch_size*seq_len0*d_model, "dump_trt/"+mLayerName+"_q_buf.txt");
    // dump2Txt(k_buf, batch_size*seq_len1*d_model, "dump_trt/"+mLayerName+"_k_buf.txt");
    // dump2Txt(v_buf, batch_size*seq_len1*d_model, "dump_trt/"+mLayerName+"_v_buf.txt");

    invokeAddQKVBiasTranspose(q_share,
                            (T*)nullptr,
                            (T*)nullptr,
                            q_buf,
                            query_weight_bias,
                            (T*)nullptr,
                            (T*)nullptr,
                            (T*)nullptr,
                            (T*)nullptr,
                            batch_size,
                            seq_len0,
                            head_num,
                            size_per_head,
                            stream);
    invokeAddQKVBiasTranspose(k_bias,
                            (T*)nullptr,
                            (T*)nullptr,
                            k_buf,
                            key_weight_bias,
                            (T*)nullptr,
                            (T*)nullptr,
                            (T*)nullptr,
                            (T*)nullptr,
                            batch_size,
                            seq_len1,
                            head_num,
                            size_per_head,
                            stream);
    invokeAddQKVBiasTranspose(v_bias,
                            (T*)nullptr,
                            (T*)nullptr,
                            v_buf,
                            value_weight_bias,
                            (T*)nullptr,
                            (T*)nullptr,
                            (T*)nullptr,
                            (T*)nullptr,
                            batch_size,
                            seq_len1,
                            head_num,
                            size_per_head,
                            stream);

    // cudaStreamSynchronize(stream);
    // dump2Txt(q_share, batch_size*seq_len0*d_model, "dump_trt/"+mLayerName+"_q_bias_trans.txt");
    // dump2Txt(k_bias, batch_size*seq_len1*d_model, "dump_trt/"+mLayerName+"_k_bias_trans.txt");
    // dump2Txt(v_bias, batch_size*seq_len1*d_model, "dump_trt/"+mLayerName+"_v_bias_trans.txt");

    cublas_wrapper->stridedBatchedGemm(CUBLAS_OP_T,
            CUBLAS_OP_N,
            seq_len1,               //m,
            seq_len0,               //n,
            size_per_head,          //k,
            (const void*)k_bias,                     //A,
            size_per_head,              //lda,
            seq_len1*size_per_head,     //strideA,
            (const void*)q_share,                    //B,
            size_per_head,              //ldb,
            seq_len0*size_per_head,     //strideB,
            (void*)qk_buf,                 //C,
            seq_len1,               //ldc,
            seq_len0*seq_len1,      //strideC,
            batch_size*head_num,    //batchCount
            1.f,                    // alpha
            0.f                     // beta
            );

    // cudaStreamSynchronize(stream);
    // dump2Txt(qk_buf, batch_size*head_num*seq_len0*seq_len1, "dump_trt/"+mLayerName+"_qk_buf.txt");
    
    T scalar = 1 / sqrtf(size_per_head * 1.0f);
    invokeMaskedSoftMax(qk_buf,
                        qk_buf,
                        enc_mask,
                        batch_size,
                        seq_len0,
                        seq_len1,
                        head_num,
                        scalar,
                        stream
        );
    // cudaStreamSynchronize(stream);
    // dump2Txt(qk_buf, batch_size*head_num*seq_len0*seq_len1, "dump_trt/"+mLayerName+"_softmax.txt");

    cublas_wrapper->stridedBatchedGemm(CUBLAS_OP_N,
            CUBLAS_OP_N,
            size_per_head,          //m,
            seq_len0,               //n,
            seq_len1,               //k,
            (const void*)v_bias,                     //A,
            size_per_head,              //lda,
            seq_len1*size_per_head,     //strideA,
            (const void*)qk_buf,                     //B,
            seq_len1,                   //ldb,
            seq_len0*seq_len1,          //strideB,
            (void*)q_buf,                  //C,
            size_per_head,          //ldc,
            seq_len0*size_per_head, //strideC,
            batch_size*head_num,    //batchCount
            1.f,                    // alpha
            0.f                     // beta
            );
    // cudaStreamSynchronize(stream);
    // dump2Txt(q_buf, batch_size*head_num*seq_len0*size_per_head, "dump_trt/"+mLayerName+"_qkv.txt");

    invokeTransposeQKV(
        out_buf, q_buf, batch_size, seq_len0, head_num, size_per_head, stream);
    // cudaStreamSynchronize(stream);
    // dump2Txt(out_buf, batch_size*head_num*seq_len0*size_per_head, "dump_trt/"+mLayerName+"_qkv_trans.txt");
    // dump2Txt(output_weight_kernel, d_model*d_model, "dump_trt/"+mLayerName+"_lw.txt");

    cublas_wrapper->Gemm(CUBLAS_OP_N,
                        CUBLAS_OP_N,
                        d_model,
                        batch_size*seq_len0,
                        d_model,
                        output_weight_kernel,
                        d_model,
                        (const T*)(out_buf),
                        d_model,
                        outputs[0],
                        d_model);
    // cudaStreamSynchronize(stream);
    // dump2Txt((float*)(outputs[0]), batch_size*head_num*seq_len0*size_per_head, "dump_trt/"+mLayerName+"_out_linear.txt");
    // add_bias_kernel<<<gridq, block, 0, stream>>>(
    //     (float*)(outputs[0]), (float*)(outputs[0]), output_weight_bias, 
    //     batch_size, seq_len0, d_model);
    invokeAddBiasResidual((T*)(outputs[0]),     //  T* output, 
                        query_in,                   //  const T* input, 
                        output_weight_bias,         //  const T* bias, 
                        batch_size * seq_len0,       //  const int m, 
                        d_model,                    //  const int n, 
                        stream
                );
    // cudaStreamSynchronize(stream);
    // dump2Txt((float*)(outputs[0]), batch_size*head_num*seq_len0*size_per_head, "dump_trt/"+mLayerName+"_out_final.txt");
    return 0;
}

int MHAPlugin::enqueue(const PluginTensorDesc*  inputDesc,
                    const PluginTensorDesc* outputDesc,
                    const void *const *     inputs,
                    void *const *           outputs,
                    void *                  workspace,
                    cudaStream_t            stream) noexcept
{
    cudaStreamSynchronize(stream);
    cublasSetStream(cublas_handle_, stream);
    cublas_wrapper->setStream(stream);
    //  input : q, enc_in, enc_lens, qw, qb, kw, kb, vw, vb, lw, lb
    int status = 0;
    const int batch_size  = inputDesc[0].dims.d[0];    // B
    const int seq_len0    = inputDesc[0].dims.d[1];    // T0 for q
    const int d_model     = inputDesc[0].dims.d[2];    // D
    const int seq_len1    = isCrossAtten ? inputDesc[1].dims.d[1] : seq_len0;    // T1 for k v
    const int head_num    = 4 ;
    const int size_per_head = 64 ;
    // printf("%s : %d %d %d %d \n", __FUNCTION__, batch_size, seq_len0, d_model, seq_len1);
    // allocator->setTrtAllocator(gpu_allocator_);
    // cudaStreamSynchronize(stream);
    if(inputDesc[0].type == DataType::kFLOAT)
    {
        if(int8_model != -1){
            mha_forward_int8<float>(inputs,
                    outputs,
                    workspace,
                    batch_size,
                    seq_len0,
                    d_model, 
                    seq_len1,  
                    head_num, 
                    size_per_head,
                    isCrossAtten,
                    scale_list,
                    cublas_wrapper,
                    cublas_handle_,
                    stream ,
                    mLayerName
                );

        }
        else{
            cublas_wrapper->setFP32GemmConfig();
            mha_forward<float>(inputs,
                        outputs,
                        workspace,
                        batch_size,
                        seq_len0,
                        d_model, 
                        seq_len1,  
                        head_num, 
                        size_per_head,
                        isCrossAtten,
                        cublas_wrapper,
                        cublas_handle_,
                        stream ,
                        mLayerName
                    );

        }
    }
    else if(inputDesc[0].type == DataType::kHALF)
    {
        if(int8_model != -1){
            mha_forward_int8<half>(inputs,
                    outputs,
                    workspace,
                    batch_size,
                    seq_len0,
                    d_model, 
                    seq_len1,  
                    head_num, 
                    size_per_head,
                    isCrossAtten,
                    scale_list,
                    cublas_wrapper,
                    cublas_handle_,
                    stream ,
                    mLayerName
                );

        }
        else{
            cublas_wrapper->setFP16GemmConfig();
            mha_forward<half>(inputs,
                        outputs,
                        workspace,
                        batch_size,
                        seq_len0,
                        d_model, 
                        seq_len1,  
                        head_num, 
                        size_per_head,
                        isCrossAtten,
                        cublas_wrapper,
                        cublas_handle_,
                        stream ,
                        mLayerName
                    );
        }
    }
    
    return status;
}

size_t MHAPlugin::getSerializationSize() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    size_t ssize = 0;
    ssize += sizeof(bool);
    return ssize;
}

void MHAPlugin::serialize(void* buffer) const noexcept
{
    // printf("%s \n", __FUNCTION__);
    char* d = static_cast<char*>(buffer);
    const char* a = d;

    writeToBuffer(d, isCrossAtten);
    // // writeToBuffer(d, mMHAMax);

    assert(d == a + getSerializationSize());
}

bool MHAPlugin::supportsFormatCombination(int32_t               pos,
                                        const PluginTensorDesc *inOut,
                                        int32_t                 nbInputs,
                                        int32_t                 nbOutputs 
                                        ) noexcept
{
    // std::cout <<"pos " << pos << " format " << (int)inOut[pos].format 
    //     << " type " << (int)inOut[pos].type << std::endl;
    // printf("%s \n", __FUNCTION__);
    switch (pos)
    {
    // case 2:
    //     return inOut[pos].type == DataType::kINT32;
    //     break;
    
    default:
        break;
    }
    return inOut[pos].format  == TensorFormat::kLINEAR && 
        inOut[pos].type == DataType::kFLOAT; // || 
}


void MHAPlugin::destroy() noexcept
{
    // This gets called when the network containing plugin is destroyed
    // delete cublas_wrapper_mutex;
    // delete cublas_algo_map;
    // delete allocator;
    // cublasLtDestroy(cublaslt_handle_);

    // printf("%s \n", __FUNCTION__);
    delete this;
}

IPluginV2DynamicExt* MHAPlugin::clone() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    auto plugin = new MHAPlugin(mLayerName, isCrossAtten);
    plugin->setPluginNamespace(mNamespace.c_str());
    // printf("clone cublas_handle_ %p \n ", plugin->cublas_handle_);
    return plugin;
}

void MHAPlugin::setPluginNamespace(const char* libNamespace) noexcept
{
    // printf("%s \n", __FUNCTION__);
    mNamespace = libNamespace;
}

const char* MHAPlugin::getPluginNamespace() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return mNamespace.c_str();
}

MHAPluginCreator::MHAPluginCreator()
{
    // Describe MHAPlugin's required PluginField arguments
    // mPluginAttributes.emplace_back(PluginField("MHAMin", nullptr, PluginFieldType::kFLOAT32, 1));
    // printf("%s \n", __FUNCTION__);
    mPluginAttributes.emplace_back(PluginField("AttentionType", nullptr, PluginFieldType::kCHAR, 4));

    // Fill PluginFieldCollection with PluginField arguments metadata
    mFC.nbFields = mPluginAttributes.size();
    mFC.fields = mPluginAttributes.data();
}

const char* MHAPluginCreator::getPluginName() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return MHA_PLUGIN_NAME;
}

const char* MHAPluginCreator::getPluginVersion() const noexcept
{
    // printf("%s \n", __FUNCTION__);
    return MHA_PLUGIN_VERSION;
}

const PluginFieldCollection* MHAPluginCreator::getFieldNames() noexcept
{
    return &mFC;
}

IPluginV2DynamicExt* MHAPluginCreator::createPlugin(const char* name, const PluginFieldCollection* fc) noexcept
{
    const PluginField* fields = fc->fields;
    // printf("createPlugin MHAPlugin, nbFields: %d \n", fc->nbFields);

    // Parse fields from PluginFieldCollection
    // assert(fc->nbFields == 2);
    bool isCrossAtten = false;
    float scale_list[128];
    for (int i = 0; i < fc->nbFields; i++)
    {
        // std::cout << fields[i].name <<std::endl;
        if (strcmp(fields[i].name, "AttentionType") == 0){
            assert(fields[i].type == PluginFieldType::kCHAR);
            //  0: self attention, 1: cross attention
            if(strcmp(reinterpret_cast<const char*>(fields[i].data), "cross") == 0)
                isCrossAtten = true;
            // printf("createPlugin AttentionType : %s , %d\n", 
            //     reinterpret_cast<const char*>(fields[i].data), 
            //     isCrossAtten);
        }
        else if(strcmp(fields[i].name, "ScaleList") == 0){
            assert(fields[i].type == PluginFieldType::kFLOAT32);

        }
    }
    return new MHAPlugin(name, isCrossAtten);
}

IPluginV2DynamicExt* MHAPluginCreator::deserializePlugin(const char* name, const void* serialData, size_t serialLength) noexcept
{
    // This object will be deleted when the network is destroyed, which will
    // call MHAPlugin::destroy()
    return new MHAPlugin(name, serialData, serialLength);
}

void MHAPluginCreator::setPluginNamespace(const char* libNamespace) noexcept
{
    mNamespace = libNamespace;
}

const char* MHAPluginCreator::getPluginNamespace() const noexcept
{
    return mNamespace.c_str();
}

}