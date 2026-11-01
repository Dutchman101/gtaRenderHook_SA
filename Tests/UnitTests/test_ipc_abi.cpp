#include <doctest/doctest.h>

#include <Engine/Common/IDeviceState.h>
#include <TestUtils/BitmapLoader.h>
#include <common_headers.h>
#include <data_desc/imgui_input_state.h>
#include <imgui.h>
#include <injection_utils/InjectorHelpers.h>
#include <ipc/MemoryReader.h>
#include <ipc/MemoryWriter.h>
#include <ipc/shared_memory_queue_client.h>
#include <material_storage.h>
#include <render_driver/imgui_win32_driver_handler.h>
#include <rw_engine/rh_backend/mesh_rendering_backend.h>
#include <rw_engine/rh_backend/raster_backend.h>
#include <rw_engine/system_funcs/rw_device_system_globals.h>
#include <rw_engine/system_funcs/start_cmd.h>
#include <rw_game_hooks.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>

namespace
{

int32_t TestMaterialSetStreamAlwaysCallback(
    uint32_t /*pluginID*/, RwPluginDataChunkAlwaysCallBack /*cb*/ )
{
    return 1;
}

class NullTestDeviceState final : public rh::engine::IDeviceState
{
  public:
    bool create_image_called = false;

    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool GetAdaptersCount( unsigned int & ) override { return true; }
    bool GetAdapterInfo( unsigned int, std::string & ) override { return true; }
    bool GetCurrentAdapter( unsigned int & ) override { return true; }
    bool SetCurrentAdapter( unsigned int ) override { return true; }
    bool GetOutputCount( unsigned int, unsigned int & ) override { return true; }
    bool GetOutputInfo( unsigned int, std::string & ) override { return true; }
    bool GetCurrentOutput( unsigned int & ) override { return true; }
    bool SetCurrentOutput( unsigned int ) override { return true; }
    bool GetDisplayModeCount( unsigned int, unsigned int & ) override
    {
        return true;
    }
    bool GetDisplayModeInfo( unsigned int,
                             rh::engine::DisplayModeInfo & ) override
    {
        return true;
    }
    bool GetCurrentDisplayMode( unsigned int & ) override { return true; }
    bool SetCurrentDisplayMode( unsigned int ) override { return true; }
    rh::engine::IWindow *
    CreateDeviceWindow( HWND, const rh::engine::OutputInfo & ) override
    {
        return nullptr;
    }
    rh::engine::ISyncPrimitive *
    CreateSyncPrimitive( rh::engine::SyncPrimitiveType ) override
    {
        return nullptr;
    }
    rh::engine::IFrameBuffer *
    CreateFrameBuffer( const rh::engine::FrameBufferCreateParams & ) override
    {
        return nullptr;
    }
    rh::engine::IRenderPass *
    CreateRenderPass( const rh::engine::RenderPassCreateParams & ) override
    {
        return nullptr;
    }
    rh::engine::ICommandBuffer *GetMainCommandBuffer() override
    {
        return nullptr;
    }
    rh::engine::ICommandBuffer *CreateCommandBuffer() override
    {
        return nullptr;
    }
    rh::engine::IPipeline *CreateRasterPipeline(
        const rh::engine::RasterPipelineCreateParams & ) override
    {
        return nullptr;
    }
    rh::engine::IShader *
    CreateShader( const rh::engine::ShaderDesc & ) override
    {
        return nullptr;
    }
    rh::engine::IBuffer *
    CreateBuffer( const rh::engine::BufferCreateInfo & ) override
    {
        return nullptr;
    }
    rh::engine::IImageBuffer *
    CreateImageBuffer( const rh::engine::ImageBufferCreateParams & ) override
    {
        create_image_called = true;
        return nullptr;
    }
    rh::engine::IImageView *
    CreateImageView( const rh::engine::ImageViewCreateInfo & ) override
    {
        return nullptr;
    }
    rh::engine::ISampler *
    CreateSampler( const rh::engine::SamplerDesc & ) override
    {
        return nullptr;
    }
    rh::engine::IDescriptorSetLayout *CreateDescriptorSetLayout(
        const rh::engine::DescriptorSetLayoutCreateParams & ) override
    {
        return nullptr;
    }
    rh::engine::IDescriptorSetAllocator *CreateDescriptorSetAllocator(
        const rh::engine::DescriptorSetAllocatorCreateParams & ) override
    {
        return nullptr;
    }
    rh::engine::IPipelineLayout *CreatePipelineLayout(
        const rh::engine::PipelineLayoutCreateParams & ) override
    {
        return nullptr;
    }
    void ExecuteCommandBuffer( rh::engine::ICommandBuffer *,
                               rh::engine::ISyncPrimitive *,
                               rh::engine::ISyncPrimitive * ) override
    {
    }
    void UpdateDescriptorSets(
        const rh::engine::DescriptorSetUpdateInfo & ) override
    {
    }
    void Wait(
        const rh::engine::ArrayProxy<rh::engine::ISyncPrimitive *> & ) override
    {
    }
    void DispatchToGPU(
        const rh::engine::ArrayProxy<rh::engine::CommandBufferSubmitInfo> & )
        override
    {
    }
    void WaitForGPU() override {}
    const rh::engine::DeviceLimitsInfo &GetLimits() override
    {
        static rh::engine::DeviceLimitsInfo limits{};
        return limits;
    }
};

} // namespace

TEST_CASE( "ipc/create_window_handle_roundtrip_over_dirty_shared_memory" )
{
    using rh::engine::DisplayModeInfo;
    using rh::rw::engine::MemoryReader;
    using rh::rw::engine::MemoryWriter;
    using rh::rw::engine::StartSystemCmdImpl;

    constexpr std::array<uintptr_t, 5> handle_values = {
        0x00000000u, 0x0012034Au, 0x7FFFFFFFu, 0x80010203u, 0xFFFFFFFFu };

    alignas( 16 ) std::array<unsigned char, 64> task_buffer{};

    for ( const uintptr_t expected_raw : handle_values )
    {
        CAPTURE( expected_raw );

        // Pre-fill the task buffer with a 1920x1080 DisplayModeInfo payload so
        // bytes 4..7 hold 1080 (0x00000438), matching the GET_VIDEO_MODE_INFO
        // task that runs right before CREATE_WINDOW on the shared-memory queue.
        task_buffer.fill( 0xCC );
        const DisplayModeInfo previous_mode{ 1920u, 1080u, 60u, 32u };
        std::memcpy( task_buffer.data(), &previous_mode,
                     sizeof( previous_mode ) );

        MemoryWriter writer( task_buffer.data() );
        StartSystemCmdImpl::SerializeWindowHandle(
            writer, reinterpret_cast<HWND>( expected_raw ) );
        CHECK( writer.Pos() == sizeof( uint64_t ) );

        uint64_t wire_value = 0;
        std::memcpy( &wire_value, task_buffer.data(), sizeof( wire_value ) );
        CHECK( wire_value == static_cast<uint64_t>( expected_raw ) );

        MemoryReader reader( task_buffer.data() );
        const HWND   decoded =
            StartSystemCmdImpl::DeserializeWindowHandle( reader );
        CHECK( reader.Pos() == sizeof( uint64_t ) );
        CHECK( reinterpret_cast<uintptr_t>( decoded ) == expected_raw );
    }
}

TEST_CASE( "ipc/shared_struct_abi_layouts" )
{
    using rh::engine::DisplayModeInfo;
    using rh::rw::engine::GeometryMaterial;
    using rh::rw::engine::GeometrySplit;
    using rh::rw::engine::MipLevelHeader;
    using rh::rw::engine::RasterHeader;
    using rh::rw::engine::SharedMemoryTaskType;
    using rh::rw::engine::VertexDescPosColorUVNormals;

    static_assert(
        std::is_same_v<std::underlying_type_t<SharedMemoryTaskType>, int64_t> );
    static_assert( sizeof( SharedMemoryTaskType ) == 8 );

    static_assert( sizeof( DisplayModeInfo ) == 16 );
    static_assert( offsetof( DisplayModeInfo, width ) == 0 );
    static_assert( offsetof( DisplayModeInfo, height ) == 4 );
    static_assert( offsetof( DisplayModeInfo, refreshRate ) == 8 );
    static_assert( offsetof( DisplayModeInfo, padding ) == 12 );

    static_assert( sizeof( RasterHeader ) == 20 );
    static_assert( offsetof( RasterHeader, mWidth ) == 0 );
    static_assert( offsetof( RasterHeader, mHeight ) == 4 );
    static_assert( offsetof( RasterHeader, mDepth ) == 8 );
    static_assert( offsetof( RasterHeader, mFormat ) == 12 );
    static_assert( offsetof( RasterHeader, mMipLevelCount ) == 16 );

    static_assert( sizeof( MipLevelHeader ) == 8 );
    static_assert( offsetof( MipLevelHeader, mSize ) == 0 );
    static_assert( offsetof( MipLevelHeader, mStride ) == 4 );

    static_assert( sizeof( GeometrySplit ) == 16 );
    static_assert( offsetof( GeometrySplit, mVertexOffset ) == 0 );
    static_assert( offsetof( GeometrySplit, mIndexOffset ) == 4 );
    static_assert( offsetof( GeometrySplit, mVertexCount ) == 8 );
    static_assert( offsetof( GeometrySplit, mIndexCount ) == 12 );

    static_assert( sizeof( GeometryMaterial ) == 16 );
    static_assert( offsetof( GeometryMaterial, mDiffuseRasterIdx ) == 0 );
    static_assert( offsetof( GeometryMaterial, mDiffuseColor ) == 8 );
    static_assert( offsetof( GeometryMaterial, mSpecular ) == 12 );

    static_assert( sizeof( VertexDescPosColorUVNormals ) == 96 );
    static_assert( offsetof( VertexDescPosColorUVNormals, bone_indices ) ==
                   80 );
    static_assert( offsetof( VertexDescPosColorUVNormals, color ) == 84 );
    static_assert( offsetof( VertexDescPosColorUVNormals, material_idx ) ==
                   88 );
    static_assert( offsetof( VertexDescPosColorUVNormals, emissive ) == 92 );

    CHECK( sizeof( DisplayModeInfo ) == 16 );
    CHECK( sizeof( RasterHeader ) == 20 );
    CHECK( sizeof( GeometryMaterial ) == 16 );
    CHECK( sizeof( VertexDescPosColorUVNormals ) == 96 );
}

TEST_CASE( "ipc/unhandled_task_does_not_crash_or_deadlock" )
{
    using rh::rw::engine::MemoryReader;
    using rh::rw::engine::MemoryWriter;
    using rh::rw::engine::SharedMemoryTaskQueue;

    SharedMemoryTaskQueue server(
        { .mName = "UnitTestUnhandledQueue", .mSize = 64 * 1024, .mOwner = true } );
    SharedMemoryTaskQueue client(
        { .mName = "UnitTestUnhandledQueue", .mSize = 64 * 1024, .mOwner = false } );

    std::atomic<bool> finished{ false };
    std::thread       server_thread( [&]() { server.TaskLoop(); } );
    std::thread       client_thread( [&]() {
        client.ExecuteTask(
            999, []( MemoryWriter && ) {}, []( MemoryReader && ) {} );
        finished.store( true, std::memory_order_release );
    } );

    client_thread.join();
    server_thread.join();
    CHECK( finished.load( std::memory_order_acquire ) );
}

TEST_CASE( "ipc/terminated_peer_process_aborts_without_deadlock" )
{
    using rh::rw::engine::IPCRenderMode;
    using rh::rw::engine::IPCSettings;
    using rh::rw::engine::MemoryReader;
    using rh::rw::engine::MemoryWriter;
    using rh::rw::engine::SharedMemoryTaskQueue;

    const auto prev_mode = IPCSettings::mMode;
    IPCSettings::mMode   = IPCRenderMode::CrossProcessClient;

    SharedMemoryTaskQueue owner_queue(
        { .mName = "UnitTestPeerExitQueue", .mSize = 64 * 1024, .mOwner = true } );

    STARTUPINFOA        si{ .cb = sizeof( si ) };
    PROCESS_INFORMATION pi{};
    char                cmd[] = "cmd.exe /c exit 0";
    REQUIRE( CreateProcessA( nullptr, cmd, nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi ) );
    WaitForSingleObject( pi.hProcess, 2000 );

    owner_queue.SetPeerProcess( pi.hProcess );
    bool       deserializer_called = false;
    const auto t0                  = std::chrono::steady_clock::now();
    owner_queue.ExecuteTask(
        777, []( MemoryWriter && ) {},
        [&]( MemoryReader && ) { deserializer_called = true; } );
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0 )
            .count();

    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    IPCSettings::mMode = prev_mode;

    CHECK_FALSE( deserializer_called );
    CHECK( elapsed_ms < 1000 );
}

TEST_CASE( "hooks/rw_game_hooks_jmp_thunk_and_return_contracts" )
{
    uint8_t *exec_buf = static_cast<uint8_t *>( VirtualAlloc(
        nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) );
    REQUIRE( exec_buf != nullptr );
    std::memset( exec_buf, 0x90, 64 );
    exec_buf[0]  = 0xE9;
    exec_buf[16] = 0xCC;

    RwDevice                       fake_device{};
    rh::rw::engine::RwPointerTable table{};
    table.mRwDevicePtr                  = reinterpret_cast<INT_PTR>( &fake_device );
    table.m_fpCheckNativeTextureSupport = reinterpret_cast<INT_PTR>( exec_buf );
    table.mMaterialRegisterPluginPtr    = 0;
    table.mMaterialSetStreamAlwaysCallbackPtr =
        reinterpret_cast<INT_PTR>( &TestMaterialSetStreamAlwaysCallback );

    rh::rw::engine::gRwDeviceGlobals.PluginFuncs
        .MaterialSetStreamAlwaysCallBack = nullptr;
    rh::rw::engine::RwGameHooks::Patch( table );

    CHECK( exec_buf[0] == 0xE9 );
    CHECK( exec_buf[5] == 0x90 );
    CHECK( rh::rw::engine::gRwDeviceGlobals.PluginFuncs
               .MaterialSetStreamAlwaysCallBack ==
           &TestMaterialSetStreamAlwaysCallback );

    using ThunkFn          = int32_t ( * )();
    const int32_t native_r = reinterpret_cast<ThunkFn>( exec_buf )();
    CHECK( native_r == 1 );
    CHECK( rh::rw::engine::RwGameHooks::SetVideoMode( 0u ) == 1 );

    VirtualFree( exec_buf, 0, MEM_RELEASE );
}

TEST_CASE( "hooks/set_int_writes_exact_four_bytes" )
{
    uint32_t words[2] = { 0x11111111u, 0xCAFEBABEu };
    SetInt( reinterpret_cast<INT_PTR>( &words[0] ), 0x22222222 );
    CHECK( words[0] == 0x22222222u );
    CHECK( words[1] == 0xCAFEBABEu );
}

TEST_CASE( "materials/spec_tex_clamped_and_null_terminated" )
{
    const auto tmp_dir =
        std::filesystem::temp_directory_path() / "rh_unit_test_materials";
    std::filesystem::create_directories( tmp_dir / "materials" );
    {
        std::ofstream out32( tmp_dir / "materials" / "exact32_test.mat" );
        out32 << R"({"spec_tex":"12345678901234567890123456789012"})";
    }
    {
        std::ofstream out64( tmp_dir / "materials" / "over64_test.mat" );
        out64 << R"({"spec_tex":"1234567890123456789012345678901234567890123456789012345678901234"})";
    }

    const auto old_cwd = std::filesystem::current_path();
    std::filesystem::current_path( tmp_dir );
    auto &ms      = rh::rw::engine::MaterialExtensionSystem::GetInstance();
    auto  desc_32 = ms.GetMatDesc( "exact32_test" );
    auto  desc_64 = ms.GetMatDesc( "over64_test" );
    std::filesystem::current_path( old_cwd );
    std::filesystem::remove_all( tmp_dir );

    REQUIRE( desc_32.has_value() );
    CHECK( desc_32->mSpecularTextureName[31] == '\0' );
    CHECK( std::strlen( desc_32->mSpecularTextureName.data() ) == 31 );

    REQUIRE( desc_64.has_value() );
    CHECK( desc_64->mSpecularTextureName[31] == '\0' );
    CHECK( std::strlen( desc_64->mSpecularTextureName.data() ) == 31 );
}

TEST_CASE( "imgui/win32_driver_preserves_qpc_frequency" )
{
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext( ctx );

    rh::rw::engine::ImGuiWin32DriverHandler handler;
    REQUIRE( handler.Init( GetDesktopWindow() ) );
    std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );

    rh::rw::engine::ImGuiInputState input{};
    handler.NewFrame( input );
    std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    handler.NewFrame( input );
    const float dt = ImGui::GetIO().DeltaTime;

    handler.Shutdown();
    ImGui::DestroyContext( ctx );

    CHECK( dt >= 0.002f );
    CHECK( dt <= 0.5f );
}

TEST_CASE( "bitmap/missing_or_invalid_bmp_fails_closed" )
{
    NullTestDeviceState       device;
    rh::engine::IImageBuffer *out_buf =
        reinterpret_cast<rh::engine::IImageBuffer *>( 0x1 );
    rh::tests::LoadBMPImage( "nonexistent_folder/missing.bmp", device,
                             &out_buf );
    CHECK_FALSE( device.create_image_called );
    CHECK( out_buf == nullptr );
}


