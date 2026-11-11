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

UseSharedSdl = host_machine.system() == 'windows'
SdlOptions = CMake.subproject_options()
SdlOptions.add_cmake_defines(PlatformCMakeDefines + {
  'SDL_SHARED': UseSharedSdl,
  'SDL_STATIC': not UseSharedSdl,
  'SDL_TEST_LIBRARY': false,
  'SDL_TESTS': false,
  'SDL_EXAMPLES': false,
  'SDL_INSTALL': false,
})
SdlProject = CMake.subproject('sdl3',
  required : Render2DOption.enabled() or AudioEnabled, options : SdlOptions)

if SdlProject.found()
  Sdl3Dependency = SdlProject.dependency(UseSharedSdl ? 'SDL3-shared' : 'SDL3-static')
endif
