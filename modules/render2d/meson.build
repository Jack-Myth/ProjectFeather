CMake = import('cmake')
PlatformCMakeDefines = {}
if host_machine.system() == 'windows'
  ResourceCompiler = find_program('rc', required : false)
  ManifestTool = find_program('mt', required : false)
  ResourceCompilerPath = ResourceCompiler.found() ? ResourceCompiler.full_path() : meson.get_external_property('cmake_rc_compiler', '')
  ManifestToolPath = ManifestTool.found() ? ManifestTool.full_path() : meson.get_external_property('cmake_manifest_tool', '')
  if ResourceCompilerPath == '' or ManifestToolPath == ''
    SdkBin = run_command('cmd.exe', '/d', '/c', 'echo %WindowsSdkVerBinPath%',
      check : false).stdout().strip()
    SdkArch = host_machine.cpu_family() == 'x86_64' ? 'x64' : host_machine.cpu_family()
    if SdkBin != '' and SdkBin != '%WindowsSdkVerBinPath%'
      if ResourceCompilerPath == ''
        ResourceCompiler = find_program(SdkBin / SdkArch / 'rc.exe', required : false)
        ResourceCompilerPath = ResourceCompiler.found() ? ResourceCompiler.full_path() : ''
      endif
      if ManifestToolPath == ''
        ManifestTool = find_program(SdkBin / SdkArch / 'mt.exe', required : false)
        ManifestToolPath = ManifestTool.found() ? ManifestTool.full_path() : ''
      endif
    endif
  endif
  if ResourceCompilerPath != '' and ManifestToolPath != ''
    ResourceCompilerPath = ResourceCompilerPath.replace('\\', '/')
    ManifestToolPath = ManifestToolPath.replace('\\', '/')
    PlatformCMakeDefines += {
      'CMAKE_RC_COMPILER': ResourceCompilerPath,
      'CMAKE_MT': ManifestToolPath,
    }
  endif
endif

SdlOptions = CMake.subproject_options()
SdlOptions.add_cmake_defines(PlatformCMakeDefines + {
  'SDL_SHARED': false,
  'SDL_STATIC': true,
  'SDL_TEST_LIBRARY': false,
  'SDL_TESTS': false,
  'SDL_EXAMPLES': false,
  'SDL_INSTALL': false,
})
SdlProject = CMake.subproject('sdl3',
  required : Render2DOption.enabled(), options : SdlOptions)

BgfxOptions = CMake.subproject_options()
BgfxOptions.add_cmake_defines(PlatformCMakeDefines + {
  'BGFX_BUILD_EXAMPLES': false,
  'BGFX_BUILD_TESTS': false,
  'BGFX_BUILD_TOOLS': false,
  'BGFX_INSTALL': false,
  'BGFX_CUSTOM_TARGETS': false,
  'BGFX_AMALGAMATED': true,
  'BGFX_CONFIG_RENDERER_WEBGPU': false,
})
BgfxProject = CMake.subproject('bgfx-cmake',
  required : Render2DOption.enabled(), options : BgfxOptions)

Render2DModuleAvailable = SdlProject.found() and BgfxProject.found()
if Render2DModuleAvailable
  Sdl3Dependency = SdlProject.dependency('SDL3-static')
  BgfxDependency = BgfxProject.dependency('bgfx')
  Render2DModuleIncludes = include_directories(
    '.',
    '../../subprojects/bgfx-cmake/bgfx/3rdparty',
    '../../subprojects/bgfx-cmake/bimg/3rdparty')
  Render2DModuleCppArgs = host_machine.system() == 'windows' ? ['/Zc:preprocessor'] : []
  Render2DModuleSources = files('Render2D.cpp', 'Render2DBgfx.cpp', 'Render2DModule.cpp')
  Render2DModuleDependencies = [FeatherDependency, Sdl3Dependency, BgfxDependency]
endif
