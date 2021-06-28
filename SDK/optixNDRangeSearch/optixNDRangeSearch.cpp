//
// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <glad/glad.h> // Needs to be included before gl_interop

#include <cuda_runtime.h>
#include <cuda_gl_interop.h>

#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stack_size.h>
#include <optix_stubs.h>

#include <sampleConfig.h>

#include <sutil/Camera.h>
#include <sutil/Trackball.h>
#include <sutil/CUDAOutputBuffer.h>
#include <sutil/Exception.h>
#include <sutil/GLDisplay.h>
#include <sutil/Matrix.h>
#include <sutil/sutil.h>
#include <sutil/vec_math.h>
#include <sutil/Timing.h>

#include <GLFW/glfw3.h>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <string>
#include <assert.h>

#include "optixNDRangeSearch.h"


//------------------------------------------------------------------------------
//
// Globals
//
//------------------------------------------------------------------------------

const int         max_trace = 12;

//------------------------------------------------------------------------------
//
// Local types
// TODO: some of these should move to sutil or optix util header
//
//------------------------------------------------------------------------------

template <typename T>
struct Record
{
    __align__( OPTIX_SBT_RECORD_ALIGNMENT )

    char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    T data;
};

typedef Record<GeomData>        RayGenRecord;
typedef Record<MissData>        MissRecord;
typedef Record<HitGroupData>    HitGroupRecord;

const uint32_t OBJ_COUNT = 4;

struct WhittedState
{
    OptixDeviceContext          context                   = 0;
    OptixTraversableHandle*     gas_handle                = nullptr;
    CUdeviceptr*                d_gas_output_buffer       = nullptr;

    OptixModule                 geometry_module           = 0;
    OptixModule                 camera_module             = 0;

    OptixProgramGroup           raygen_prog_group         = 0;
    OptixProgramGroup           radiance_miss_prog_group  = 0;
    OptixProgramGroup           radiance_metal_sphere_prog_group  = 0;

    OptixPipeline               pipeline                  = 0;
    OptixPipelineCompileOptions pipeline_compile_options  = {};

    CUstream                    stream                    = 0;
    Params                      params;
    Params*                     d_params                  = nullptr;

    //float3**                    d_spheres                 = nullptr;
    float3**                    points = nullptr;
    float3**                    ndpoints = nullptr;

    int                         dim = 3;
    int                         batch = 1;

    OptixShaderBindingTable     sbt                       = {};
};

//------------------------------------------------------------------------------
//
//  Helper functions
//
//------------------------------------------------------------------------------

int tokenize(std::string s, std::string del, float3** ndpoints, unsigned int lineId)
{
  int start = 0;
  int end = s.find(del);
  int dim = 0;

  std::vector<float> vcoords;
  while (end != -1) {
    float coord = std::stof(s.substr(start, end - start));
    //std::cout << coord << std::endl;
    if (ndpoints != nullptr) {
      vcoords.push_back(coord);
    }
    start = end + del.size();
    end = s.find(del, start);
    dim++;
  }
  float coord  = std::stof(s.substr(start, end - start));
  //std::cout << coord << std::endl;
  if (ndpoints != nullptr) {
    vcoords.push_back(coord);
  }
  dim++;

  assert(dim > 0);
  if ((dim % 3) != 0) dim = (dim/3+1)*3;

  if (ndpoints != nullptr) {
    for (int batch = 0; batch < dim/3; batch++) {
      float3 point = make_float3(vcoords[batch*3], vcoords[batch*3+1], vcoords[batch*3+2]);
      ndpoints[batch][lineId] = point;
    }
  }

  return dim;
}

float3** read_pc_data(const char* data_file, unsigned int* N, int* d) {
  std::ifstream file;

  file.open(data_file);
  if( !file.good() ) {
    std::cerr << "Could not read the frame data...\n";
    assert(0);
  }

  char line[1024];
  unsigned int lines = 0;
  int dim = 0;

  while (file.getline(line, 1024)) {
    if (lines == 0) {
      std::string str(line);
      dim = tokenize(str, ",", nullptr, 0);
    }
    lines++;
  }
  file.clear();
  file.seekg(0, std::ios::beg);

  *N = lines;
  *d = dim;

  float3** ndpoints = new float3*[dim/3];
  for (int i = 0; i < dim/3; i++) {
    ndpoints[i] = new float3[lines];
  }

  lines = 0;
  while (file.getline(line, 1024)) {
    std::string str(line);
    tokenize(str, ",", ndpoints, lines);

    //std::cerr << ndpoints[0][lines].x << "," << ndpoints[0][lines].y << "," << ndpoints[0][lines].z << std::endl;
    //std::cerr << ndpoints[1][lines].x << "," << ndpoints[1][lines].y << "," << ndpoints[1][lines].z << std::endl;
    lines++;
  }

  file.close();

  return ndpoints;
}

void printUsageAndExit( const char* argv0 )
{
    std::cerr << "Usage  : " << argv0 << " [options]\n";
    std::cerr << "Options: --file | -f <filename>      File for point cloud input\n";
    std::cerr << "         --radius | -r               Search radius\n";
    std::cerr << "         --knn | -k                  Max K returned\n";
    std::cerr << "         --help | -h                 Print this usage message\n";
    exit( 0 );
}

void initLaunchParams( WhittedState& state )
{
    state.params.frame_buffer = nullptr; // Will be set when output buffer is mapped

    state.params.max_depth = max_trace;
    state.params.scene_epsilon = 1.e-4f;
}

static void sphere_bound(float3 center, float radius, float result[6])
{
    OptixAabb *aabb = reinterpret_cast<OptixAabb*>(result);

    float3 m_min = center - radius;
    float3 m_max = center + radius;

    *aabb = {
        m_min.x, m_min.y, m_min.z,
        m_max.x, m_max.y, m_max.z
    };
}

static void buildGas(
    const WhittedState &state,
    const OptixAccelBuildOptions &accel_options,
    const OptixBuildInput &build_input,
    OptixTraversableHandle &gas_handle,
    CUdeviceptr &d_gas_output_buffer
    )
{
    OptixAccelBufferSizes gas_buffer_sizes;
    CUdeviceptr d_temp_buffer_gas;

    OPTIX_CHECK( optixAccelComputeMemoryUsage(
        state.context,
        &accel_options,
        &build_input,
        1,
        &gas_buffer_sizes));

    CUDA_CHECK( cudaMalloc(
        reinterpret_cast<void**>( &d_temp_buffer_gas ),
        gas_buffer_sizes.tempSizeInBytes));

    // non-compacted output and size of compacted GAS
    CUdeviceptr d_buffer_temp_output_gas_and_compacted_size;
    size_t compactedSizeOffset = roundUp<size_t>( gas_buffer_sizes.outputSizeInBytes, 8ull );
    CUDA_CHECK( cudaMalloc(
                reinterpret_cast<void**>( &d_buffer_temp_output_gas_and_compacted_size ),
                compactedSizeOffset + 8
                ) );

    OptixAccelEmitDesc emitProperty = {};
    emitProperty.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
    emitProperty.result = (CUdeviceptr)((char*)d_buffer_temp_output_gas_and_compacted_size + compactedSizeOffset);

    OPTIX_CHECK( optixAccelBuild(
        state.context,
        state.stream,
        &accel_options,
        &build_input,
        1,
        d_temp_buffer_gas,
        gas_buffer_sizes.tempSizeInBytes,
        d_buffer_temp_output_gas_and_compacted_size,
        gas_buffer_sizes.outputSizeInBytes,
        &gas_handle,
        &emitProperty,
        1) );

    CUDA_CHECK( cudaFree( (void*)d_temp_buffer_gas ) );

    size_t compacted_gas_size;
    CUDA_CHECK( cudaMemcpy( &compacted_gas_size, (void*)emitProperty.result, sizeof(size_t), cudaMemcpyDeviceToHost ) );

    if( compacted_gas_size < gas_buffer_sizes.outputSizeInBytes )
    {
        CUDA_CHECK( cudaMalloc( reinterpret_cast<void**>( &d_gas_output_buffer ), compacted_gas_size ) );

        // use handle as input and output
        OPTIX_CHECK( optixAccelCompact( state.context, state.stream, gas_handle, d_gas_output_buffer, compacted_gas_size, &gas_handle ) );

        CUDA_CHECK( cudaFree( (void*)d_buffer_temp_output_gas_and_compacted_size ) );
    }
    else
    {
        d_gas_output_buffer = d_buffer_temp_output_gas_and_compacted_size;
    }
}

void createGeometry( WhittedState &state )
{
    //
    // Allocate device memory for the spheres (points/queries)
    //

    state.gas_handle = new OptixTraversableHandle[state.batch];
    state.d_gas_output_buffer = new CUdeviceptr[state.batch];
    for (int b = 0; b < state.batch; b++) {
       //float3* spheres;
       CUDA_CHECK( cudaMalloc(
           reinterpret_cast<void**>(&state.params.points[b]),
           //reinterpret_cast<void**>(&spheres),
           //reinterpret_cast<void**>(&state.d_spheres),
           state.params.numPrims * sizeof(float3) ) );

       CUDA_CHECK( cudaMemcpy(
           reinterpret_cast<void*>(state.params.points[b]),
           //reinterpret_cast<void*>(spheres),
           //reinterpret_cast<void*>( state.d_spheres),
           state.ndpoints[b],
           state.params.numPrims * sizeof(float3),
           cudaMemcpyHostToDevice
       ) );
       //state.params.points[b] = spheres;
    }

    //
    // Build Custom Primitives
    //

    for (int b = 0; b < state.batch; b++) {
      // Load AABB into device memory
      OptixAabb* aabb = (OptixAabb*)malloc(state.params.numPrims * sizeof(OptixAabb));
      CUdeviceptr d_aabb;

      for(unsigned int i = 0; i < state.params.numPrims; i++) {
        sphere_bound(
            state.ndpoints[b][i], state.params.radius,
            reinterpret_cast<float*>(&aabb[i]));
      }

      CUDA_CHECK( cudaMalloc( reinterpret_cast<void**>( &d_aabb
          ), state.params.numPrims* sizeof( OptixAabb ) ) );
      CUDA_CHECK( cudaMemcpyAsync(
                  reinterpret_cast<void*>( d_aabb ),
                  aabb,
                  state.params.numPrims * sizeof( OptixAabb ),
                  cudaMemcpyHostToDevice,
                  state.stream
      ) );

      // Setup AABB build input
      uint32_t* aabb_input_flags = (uint32_t*)malloc(state.params.numPrims * sizeof(uint32_t));

      for (unsigned int i = 0; i < state.params.numPrims; i++) {
        //aabb_input_flags[i] = OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;
        aabb_input_flags[i] = OPTIX_GEOMETRY_FLAG_NONE;
      }

      OptixBuildInput aabb_input = {};
      aabb_input.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
      aabb_input.customPrimitiveArray.aabbBuffers   = &d_aabb;
      aabb_input.customPrimitiveArray.flags         = aabb_input_flags;
      aabb_input.customPrimitiveArray.numSbtRecords = 1;
      aabb_input.customPrimitiveArray.numPrimitives = state.params.numPrims;
      // it's important to pass 0 to sbtIndexOffsetBuffer
      aabb_input.customPrimitiveArray.sbtIndexOffsetBuffer         = 0;
      aabb_input.customPrimitiveArray.sbtIndexOffsetSizeInBytes    = sizeof( uint32_t );
      aabb_input.customPrimitiveArray.primitiveIndexOffset         = 0;

      OptixAccelBuildOptions accel_options = {
          OPTIX_BUILD_FLAG_ALLOW_COMPACTION,  // buildFlags
          OPTIX_BUILD_OPERATION_BUILD         // operation
      };

      buildGas(
          state,
          accel_options,
          aabb_input,
          state.gas_handle[b],
          state.d_gas_output_buffer[b]);

      CUDA_CHECK( cudaFree( (void*)d_aabb) );
      free(aabb);
    }
}

void createModules( WhittedState &state )
{
    OptixModuleCompileOptions module_compile_options = {
        100,                                    // maxRegisterCount
        OPTIX_COMPILE_OPTIMIZATION_DEFAULT,     // optLevel
        OPTIX_COMPILE_DEBUG_LEVEL_NONE          // debugLevel
    };
    char log[2048];
    size_t sizeof_log = sizeof(log);

    {
        const std::string ptx = sutil::getPtxString( OPTIX_SAMPLE_NAME, OPTIX_SAMPLE_DIR, "geometry.cu" );
        OPTIX_CHECK_LOG( optixModuleCreateFromPTX(
            state.context,
            &module_compile_options,
            &state.pipeline_compile_options,
            ptx.c_str(),
            ptx.size(),
            log,
            &sizeof_log,
            &state.geometry_module ) );
    }

    {
        const std::string ptx = sutil::getPtxString( OPTIX_SAMPLE_NAME, OPTIX_SAMPLE_DIR, "camera.cu" );
        OPTIX_CHECK_LOG( optixModuleCreateFromPTX(
            state.context,
            &module_compile_options,
            &state.pipeline_compile_options,
            ptx.c_str(),
            ptx.size(),
            log,
            &sizeof_log,
            &state.camera_module ) );
    }
}

static void createCameraProgram( WhittedState &state, std::vector<OptixProgramGroup> &program_groups )
{
    OptixProgramGroup           cam_prog_group;
    OptixProgramGroupOptions    cam_prog_group_options = {};
    OptixProgramGroupDesc       cam_prog_group_desc = {};
    cam_prog_group_desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    cam_prog_group_desc.raygen.module = state.camera_module;
    cam_prog_group_desc.raygen.entryFunctionName = "__raygen__pinhole_camera";

    char    log[2048];
    size_t  sizeof_log = sizeof( log );
    OPTIX_CHECK_LOG( optixProgramGroupCreate(
        state.context,
        &cam_prog_group_desc,
        1,
        &cam_prog_group_options,
        log,
        &sizeof_log,
        &cam_prog_group ) );

    program_groups.push_back(cam_prog_group);
    state.raygen_prog_group = cam_prog_group;
}

static void createMetalSphereProgram( WhittedState &state, std::vector<OptixProgramGroup> &program_groups )
{
    char    log[2048];
    size_t  sizeof_log = sizeof( log );

    OptixProgramGroup           radiance_sphere_prog_group;
    OptixProgramGroupOptions    radiance_sphere_prog_group_options = {};
    OptixProgramGroupDesc       radiance_sphere_prog_group_desc = {};
    radiance_sphere_prog_group_desc.kind   = OPTIX_PROGRAM_GROUP_KIND_HITGROUP,
    radiance_sphere_prog_group_desc.hitgroup.moduleIS               = state.geometry_module;
    radiance_sphere_prog_group_desc.hitgroup.entryFunctionNameIS    = "__intersection__sphere";
    radiance_sphere_prog_group_desc.hitgroup.moduleCH               = nullptr;
    radiance_sphere_prog_group_desc.hitgroup.entryFunctionNameCH    = nullptr;
    radiance_sphere_prog_group_desc.hitgroup.moduleAH               = state.geometry_module;
    radiance_sphere_prog_group_desc.hitgroup.entryFunctionNameAH    = "__anyhit__terminateRay";

    OPTIX_CHECK_LOG( optixProgramGroupCreate(
        state.context,
        &radiance_sphere_prog_group_desc,
        1,
        &radiance_sphere_prog_group_options,
        log,
        &sizeof_log,
        &radiance_sphere_prog_group ) );

    program_groups.push_back(radiance_sphere_prog_group);
    state.radiance_metal_sphere_prog_group = radiance_sphere_prog_group;
}

static void createMissProgram( WhittedState &state, std::vector<OptixProgramGroup> &program_groups )
{
    OptixProgramGroupOptions    miss_prog_group_options = {};
    OptixProgramGroupDesc       miss_prog_group_desc = {};
    miss_prog_group_desc.kind   = OPTIX_PROGRAM_GROUP_KIND_MISS;
    miss_prog_group_desc.miss.module             = nullptr;
    miss_prog_group_desc.miss.entryFunctionName  = nullptr;

    char    log[2048];
    size_t  sizeof_log = sizeof( log );
    OPTIX_CHECK_LOG( optixProgramGroupCreate(
        state.context,
        &miss_prog_group_desc,
        1,
        &miss_prog_group_options,
        log,
        &sizeof_log,
        &state.radiance_miss_prog_group ) );

    program_groups.push_back(state.radiance_miss_prog_group);
}

void createPipeline( WhittedState &state )
{
    std::vector<OptixProgramGroup> program_groups;

    state.pipeline_compile_options = {
        false,                                                  // usesMotionBlur
        OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS,          // traversableGraphFlags
        2,                                                      // numPayloadValues
        0,                                                      // numAttributeValues
        OPTIX_EXCEPTION_FLAG_NONE,                              // exceptionFlags
        "params"                                                // pipelineLaunchParamsVariableName
    };

    // Prepare program groups
    createModules( state );
    createCameraProgram( state, program_groups );
    createMetalSphereProgram( state, program_groups );
    createMissProgram( state, program_groups );

    // Link program groups to pipeline
    OptixPipelineLinkOptions pipeline_link_options = {
        max_trace,                          // maxTraceDepth
        OPTIX_COMPILE_DEBUG_LEVEL_FULL      // debugLevel
    };
    char    log[2048];
    size_t  sizeof_log = sizeof(log);
    OPTIX_CHECK_LOG( optixPipelineCreate(
        state.context,
        &state.pipeline_compile_options,
        &pipeline_link_options,
        program_groups.data(),
        static_cast<unsigned int>( program_groups.size() ),
        log,
        &sizeof_log,
        &state.pipeline ) );

    OptixStackSizes stack_sizes = {};
    for( auto& prog_group : program_groups )
    {
        OPTIX_CHECK( optixUtilAccumulateStackSizes( prog_group, &stack_sizes ) );
    }

    uint32_t direct_callable_stack_size_from_traversal;
    uint32_t direct_callable_stack_size_from_state;
    uint32_t continuation_stack_size;
    OPTIX_CHECK( optixUtilComputeStackSizes( &stack_sizes, max_trace,
                                             0,  // maxCCDepth
                                             0,  // maxDCDepth
                                             &direct_callable_stack_size_from_traversal,
                                             &direct_callable_stack_size_from_state, &continuation_stack_size ) );
    OPTIX_CHECK( optixPipelineSetStackSize( state.pipeline, direct_callable_stack_size_from_traversal,
                                            direct_callable_stack_size_from_state, continuation_stack_size,
                                            1  // maxTraversableDepth
                                            ) );
}

void createSBT( WhittedState &state )
{
    // Raygen program record
    // We need no raygen record, so a dummy record here.
    {
        CUdeviceptr d_raygen_record;
        CUDA_CHECK( cudaMalloc(
            reinterpret_cast<void**>( &d_raygen_record ),
            sizeof( RayGenRecord ) ) );

        RayGenRecord rg_sbt;
        optixSbtRecordPackHeader( state.raygen_prog_group, &rg_sbt );
        //rg_sbt.data.spheres = state.d_spheres;

        CUDA_CHECK( cudaMemcpy(
            reinterpret_cast<void*>( d_raygen_record ),
            &rg_sbt,
            sizeof(rg_sbt),
            cudaMemcpyHostToDevice
        ) );

        state.sbt.raygenRecord = d_raygen_record;
    }

    // Miss program record
    // We need no miss record, so a dummy record here.
    {
        CUdeviceptr d_miss_record;
        size_t sizeof_miss_record = sizeof( MissRecord );
        CUDA_CHECK( cudaMalloc(
            reinterpret_cast<void**>( &d_miss_record ),
            sizeof_miss_record*RAY_TYPE_COUNT ) );

        MissRecord ms_sbt;
        optixSbtRecordPackHeader( state.radiance_miss_prog_group, &ms_sbt );

        CUDA_CHECK( cudaMemcpy(
            reinterpret_cast<void*>( d_miss_record ),
            &ms_sbt,
            sizeof_miss_record,
            cudaMemcpyHostToDevice
        ) );

        state.sbt.missRecordBase          = d_miss_record;
        state.sbt.missRecordCount         = 1;
        state.sbt.missRecordStrideInBytes = static_cast<uint32_t>( sizeof_miss_record );
    }

    // Hitgroup program record
    // We need no hit record, so a dummy record here.
    {
        CUdeviceptr d_hitgroup_records;
        size_t      sizeof_hitgroup_record = sizeof( HitGroupRecord );
        CUDA_CHECK( cudaMalloc(
            reinterpret_cast<void**>( &d_hitgroup_records ),
            sizeof_hitgroup_record
        ) );

        HitGroupRecord hit_sbt;
        OPTIX_CHECK( optixSbtRecordPackHeader(
            state.radiance_metal_sphere_prog_group,
            &hit_sbt ) );

        CUDA_CHECK( cudaMemcpy(
            reinterpret_cast<void*>( d_hitgroup_records ),
            &hit_sbt,
            sizeof_hitgroup_record,
            cudaMemcpyHostToDevice
        ) );

        state.sbt.hitgroupRecordBase            = d_hitgroup_records;
        state.sbt.hitgroupRecordCount           = 1;
        state.sbt.hitgroupRecordStrideInBytes   = static_cast<uint32_t>( sizeof_hitgroup_record );
    }
}

//static void context_log_cb( unsigned int level, const char* tag, const char* message, void* /*cbdata */)
//{
//    std::cerr << "[" << std::setw( 2 ) << level << "][" << std::setw( 12 ) << tag << "]: "
//              << message << "\n";
//}

void createContext( WhittedState& state )
{
    // Initialize CUDA
    CUDA_CHECK( cudaFree( 0 ) );

    OptixDeviceContext context;
    CUcontext          cuCtx = 0;  // zero means take the current context
    OPTIX_CHECK( optixInit() );
    OptixDeviceContextOptions options = {};
    //options.logCallbackFunction       = &context_log_cb;
    options.logCallbackFunction       = nullptr;
    options.logCallbackLevel          = 4;
    OPTIX_CHECK( optixDeviceContextCreate( cuCtx, &options, &context ) );

    state.context = context;
}

void launchSubframe( sutil::CUDAOutputBuffer<unsigned int>& output_buffer, WhittedState& state, int batch )
{
    // Launch
    // this map() thing basically returns the cudaMalloc-ed device pointer.
    unsigned int* result_buffer_data = output_buffer.map();

    // need to manually set the cuda-malloced device memory. note the semantics
    // of cudamemset: it sets #count number of BYTES to value; literally think
    // about what each byte has to be.
    CUDA_CHECK( cudaMemsetAsync ( result_buffer_data, 0xFF, state.params.numPrims*state.params.knn*sizeof(unsigned int), state.stream ) );
    state.params.frame_buffer = result_buffer_data;
    state.params.queries = state.params.points[batch];
    state.params.handle = state.gas_handle[batch];
    state.params.batchId = batch;

    //std::cout << state.params.handle << std::endl;
    //std::cout << state.params.frame_buffer << std::endl;
    //std::cout << state.params.queries << std::endl;
    //std::cout << state.params.points << std::endl;

    CUDA_CHECK( cudaMalloc( reinterpret_cast<void**>( &state.d_params ), sizeof( Params ) ) );

    CUDA_CHECK( cudaMemcpyAsync( reinterpret_cast<void*>( state.d_params ),
                                 &state.params,
                                 sizeof( Params ),
                                 cudaMemcpyHostToDevice,
                                 state.stream
    ) );

    OPTIX_CHECK( optixLaunch(
        state.pipeline,
        state.stream,
        reinterpret_cast<CUdeviceptr>( state.d_params ),
        sizeof( Params ),
        &state.sbt,
        state.params.numPrims, // launch width
        //state.params.numPrims/2, // launch width
        1,                     // launch height
        1                      // launch depth
    ) );
    //output_buffer.unmap();
    //CUDA_SYNC_CHECK();

    //CUstream stream1 = 0;
    //CUDA_CHECK( cudaStreamCreate( &stream1 ) );
    //state.params.frame_buffer += state.params.numPrims * state.params.knn / 2;
    //state.params.queries += state.params.numPrims / 2;
    //CUDA_CHECK( cudaMemcpyAsync( reinterpret_cast<void*>( state.d_params ),
    //                             &state.params,
    //                             sizeof( Params ),
    //                             cudaMemcpyHostToDevice,
    //                             stream1
    //) );

    //OPTIX_CHECK( optixLaunch(
    //    state.pipeline,
    //    stream1,
    //    reinterpret_cast<CUdeviceptr>( state.d_params ),
    //    sizeof( Params ),
    //    &state.sbt,
    //    state.params.numPrims -  state.params.numPrims/2, // launch width
    //    1,                     // launch height
    //    1                      // launch depth
    //) );
    output_buffer.unmap();
    CUDA_SYNC_CHECK();
}


void cleanupState( WhittedState& state )
{
    OPTIX_CHECK( optixPipelineDestroy     ( state.pipeline                ) );
    OPTIX_CHECK( optixProgramGroupDestroy ( state.raygen_prog_group       ) );
    OPTIX_CHECK( optixProgramGroupDestroy ( state.radiance_metal_sphere_prog_group ) );
    OPTIX_CHECK( optixProgramGroupDestroy ( state.radiance_miss_prog_group         ) );
    OPTIX_CHECK( optixModuleDestroy       ( state.geometry_module         ) );
    OPTIX_CHECK( optixModuleDestroy       ( state.camera_module           ) );
    OPTIX_CHECK( optixDeviceContextDestroy( state.context                 ) );

    CUDA_CHECK( cudaFree( reinterpret_cast<void*>( state.sbt.raygenRecord       ) ) );
    CUDA_CHECK( cudaFree( reinterpret_cast<void*>( state.sbt.missRecordBase     ) ) );
    CUDA_CHECK( cudaFree( reinterpret_cast<void*>( state.sbt.hitgroupRecordBase ) ) );
    for (int b = 0; b < state.batch; b++) {
      CUDA_CHECK( cudaFree( reinterpret_cast<void*>( state.d_gas_output_buffer[b]  ) ) );
    }
    //CUDA_CHECK( cudaFree( reinterpret_cast<void*>( state.d_gas_output_buffer    ) ) );
    //CUDA_CHECK( cudaFree( reinterpret_cast<void*>( state.gas_handle             ) ) );
    delete state.d_gas_output_buffer;
    delete state.gas_handle;
    CUDA_CHECK( cudaFree( reinterpret_cast<void*>( state.d_params               ) ) );
    //CUDA_CHECK( cudaFree( reinterpret_cast<void*>( state.d_spheres              ) ) );

    for (int i = 0; i < state.batch; i++) {
      delete state.ndpoints[i];
    }
    delete state.ndpoints;
}

int main( int argc, char* argv[] )
{
    WhittedState state;
    // will be overwritten if set explicitly
    state.params.radius = 2;
    state.params.knn = 50;
    std::string outfile;

    for( int i = 1; i < argc; ++i )
    {
        const std::string arg = argv[i];
        if( arg == "--help" || arg == "-h" )
        {
            printUsageAndExit( argv[0] );
        }
        else if( arg == "--file" || arg == "-f" )
        {
            if( i >= argc - 1 )
                printUsageAndExit( argv[0] );
            outfile = argv[++i];
        }
        else if( arg == "--knn" || arg == "-k" )
        {
            if( i >= argc - 1 )
                printUsageAndExit( argv[0] );
            state.params.knn = atoi(argv[++i]);
        }
        else if( arg == "--radius" || arg == "-r" )
        {
            if( i >= argc - 1 )
                printUsageAndExit( argv[0] );
            state.params.radius = std::stof(argv[++i]);
        }
        else
        {
            std::cerr << "Unknown option '" << argv[i] << "'\n";
            printUsageAndExit( argv[0] );
        }
    }

    // read points
    state.ndpoints = read_pc_data(outfile.c_str(), &state.params.numPrims, &state.dim);
    state.batch = state.dim/3;

    if (state.dim <= 0 || state.dim > MAX_DIM) {
      printUsageAndExit( argv[0] );
    }

    std::cerr << "dim: " << state.dim << std::endl;
    std::cerr << "batch: " << state.batch << std::endl;
    std::cerr << "numPrims: " << state.params.numPrims << std::endl;
    std::cerr << "radius: " << state.params.radius << std::endl;
    std::cerr << "K: " << state.params.knn << std::endl;

    //std::cerr << state.ndpoints[0][0].x << "," << state.ndpoints[0][0].y << "," << state.ndpoints[0][0].z << std::endl;
    //std::cerr << state.ndpoints[0][1].x << "," << state.ndpoints[0][1].y << "," << state.ndpoints[0][1].z << std::endl;
    //std::cerr << state.ndpoints[0][2].x << "," << state.ndpoints[0][2].y << "," << state.ndpoints[0][2].z << std::endl;
    //std::cerr << state.ndpoints[1][0].x << "," << state.ndpoints[1][0].y << "," << state.ndpoints[1][0].z << std::endl;
    //std::cerr << state.ndpoints[1][1].x << "," << state.ndpoints[1][1].y << "," << state.ndpoints[1][1].z << std::endl;
    //std::cerr << state.ndpoints[1][2].x << "," << state.ndpoints[1][2].y << "," << state.ndpoints[1][2].z << std::endl;

    Timing::reset();

    try
    {
        //
        // Set up CUDA device and stream
        //
        int32_t device_count = 0;
        CUDA_CHECK( cudaGetDeviceCount( &device_count ) );
        std::cerr << "Total GPUs visible: " << device_count << std::endl;
  
        int32_t device_id = 1;
        cudaDeviceProp prop;
        CUDA_CHECK( cudaGetDeviceProperties ( &prop, device_id ) );
        CUDA_CHECK( cudaSetDevice( device_id ) );
        std::cerr << "\t[" << device_id << "]: " << prop.name << std::endl;

        CUDA_CHECK( cudaStreamCreate( &state.stream ) );

        //
        // Set up OptiX state
        //
        Timing::startTiming("create Context");
        createContext  ( state );
        Timing::stopTiming(true);

        Timing::startTiming("create Geometry");
        createGeometry ( state );
        Timing::stopTiming(true);

        Timing::startTiming("create Pipeline");
        createPipeline ( state );
        Timing::stopTiming(true);

        Timing::startTiming("create SBT");
        createSBT      ( state );
        Timing::stopTiming(true);

        //
        // Do the work
        //

        Timing::startTiming("compute");
        initLaunchParams( state );

        sutil::CUDAOutputBufferType output_buffer_type = sutil::CUDAOutputBufferType::CUDA_DEVICE;

        sutil::CUDAOutputBuffer<unsigned int> output_buffer(
                output_buffer_type,
                state.params.numPrims*state.params.knn,
                1,
                device_id
                );

        launchSubframe( output_buffer, state, 0 );
        Timing::stopTiming(true);

        //Timing::startTiming("create second Geometry");
        //createGeometry ( state, 1 );
        //Timing::stopTiming(true);

        //Timing::startTiming("optixLaunch second compute time");
        //sutil::CUDAOutputBuffer<unsigned int> output_buffer2(
        //        output_buffer_type,
        //        state.params.numPrims*state.params.knn,
        //        1,
        //        device_id
        //        );

        //launchSubframe( output_buffer2, state );
        //Timing::stopTiming(true);

        //
        // Check the work
        //

        Timing::startTiming("Neighbor copy from device to host");
        void* data = output_buffer.getHostPointer();
        //void* data1 = output_buffer2.getHostPointer();
        Timing::stopTiming(true);

        unsigned int totalNeighbors = 0;
        unsigned int totalWrongNeighbors = 0;
        double totalWrongDist = 0;
        for (unsigned int q = 0; q < state.params.numPrims; q++) {
          for (unsigned int j = 0; j < state.params.knn; j++) {
            unsigned int p = reinterpret_cast<unsigned int*>( data )[ q * state.params.knn + j ];
            if (p == UINT_MAX) break;
            else {
              totalNeighbors++;
              float3 diff = state.ndpoints[0][p] - state.ndpoints[0][q];
              float dists = dot(diff, diff);
              if (dists > state.params.radius*state.params.radius) {
                //fprintf(stdout, "Point %u [%f, %f, %f] is not a neighbor of query %u [%f, %f, %f]. Dist is %lf.\n", p, state.points[p].x, state.points[p].y, state.points[p].z, i, state.points[i].x, state.points[i].y, state.points[i].z, sqrt(dists));
                totalWrongNeighbors++;
                totalWrongDist += sqrt(dists);
                //exit(1);
              }
            }
            std::cout << p << " ";
          }
          std::cout << "\n";
        }
        std::cerr << "Sanity check done." << std::endl;
        std::cerr << "Avg neighbor/query: " << totalNeighbors/state.params.numPrims << std::endl;
        std::cerr << "Avg wrong neighbor/query: " << totalWrongNeighbors/state.params.numPrims << std::endl;
        if (totalWrongNeighbors != 0) std::cerr << "Avg wrong dist: " << totalWrongDist / totalWrongNeighbors << std::endl;

        cleanupState( state );
    }
    catch( std::exception& e )
    {
        std::cerr << "Caught exception: " << e.what() << "\n";
        return 1;
    }

    return 0;
}