if host_machine.system() == 'windows'
  NativeModuleSuffix = 'dll'
elif host_machine.system() == 'darwin'
  NativeModuleSuffix = 'dylib'
else
  NativeModuleSuffix = 'so'
endif

ThreadsNativeModule = shared_module('threads.felib', 'threads/Threads.cpp',
  name_prefix : '',
  name_suffix : NativeModuleSuffix,
  dependencies : [FeatherDependency, ThreadDependency],
  install : true,
  install_dir : get_option('bindir') / 'modules')

StdioNativeModule = shared_module('stdio.felib', '../stdlib/StdIo.cpp',
  '../stdlib/StdIoModule.cpp',
  name_prefix : '',
  name_suffix : NativeModuleSuffix,
  dependencies : FeatherDependency,
  install : true,
  install_dir : get_option('bindir') / 'modules')



subdir('time')
TimeNativeModule = shared_module('time.felib', TimeModuleSources,
  name_prefix : '',
  name_suffix : NativeModuleSuffix,
  dependencies : FeatherDependency,
  install : true,
  install_dir : get_option('bindir') / 'modules')

Render2DOption = get_option('render2d')
AudioEnabled = get_option('audio')
if not Render2DOption.disabled() or AudioEnabled
  subdir('sdl3')
endif
if not Render2DOption.disabled()
  subdir('render2d')
endif

SdlRuntimeDependencies = []
if host_machine.system() == 'windows' and (AudioEnabled or
    (not Render2DOption.disabled() and Render2DModuleAvailable))
  SdlSharedTarget = SdlProject.target('SDL3-shared')
  Fs = import('fs')
  SdlRuntimeName = Fs.name(SdlSharedTarget.full_path())
  SdlRuntimeDependencies = [custom_target(SdlRuntimeName,
    input : SdlSharedTarget, output : SdlRuntimeName,
    command : [find_program('powershell.exe'), '-NoProfile', '-ExecutionPolicy', 'Bypass',
      '-File', files('../scripts/copy-build-file.ps1'), '@INPUT@', '@OUTPUT@'],
    build_by_default : true, install : true, install_dir : get_option('bindir') / 'modules')]
endif

if not Render2DOption.disabled()
  if Render2DModuleAvailable
    Render2DNativeModule = shared_module('render2d.felib',
      Render2DModuleSources,
      name_prefix : '',
      name_suffix : NativeModuleSuffix,
      include_directories : Render2DModuleIncludes,
      cpp_args : Render2DModuleCppArgs,
      dependencies : Render2DModuleDependencies,
      link_depends : SdlRuntimeDependencies,
      install : true,
      install_dir : get_option('bindir') / 'modules')
  endif
endif

if AudioEnabled
  subdir('audio')
  AudioNativeModule = shared_module('audio.felib', AudioSource,
    name_prefix : '', name_suffix : NativeModuleSuffix,
    dependencies : [FeatherDependency, Sdl3Dependency, OpusFile],
    link_depends : SdlRuntimeDependencies,
    install : true, install_dir : get_option('bindir') / 'modules')
endif

foreach Name : ['delegate']
  Source = configure_file(input : '../script_modules/' + Name + '.fe',
    output : Name + '.fe', copy : true, install : true,
    install_dir : get_option('bindir') / 'modules')
  custom_target('script-module-' + Name, input : Source, output : Name + '.fbc',
    command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default : true, install : true,
    install_dir : get_option('bindir') / 'modules')
endforeach

HelperSource = configure_file(input : '../tests/fixtures/helper.fe',
  output : 'helper.fe', copy : true)
configure_file(input : '../tests/fixtures/stdio-shadow.fe',
  output : 'stdio.fe', copy : true)
HelperBytecode = custom_target('helper-bytecode',
  input : '../tests/fixtures/helper.fe', output : 'helper.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true)

if get_option('network')
  subdir('network')
  NetworkNativeModule = shared_module('network.felib', NetworkSources, NetworkEntrySource,
    name_prefix : '', name_suffix : NativeModuleSuffix,
    dependencies : NetworkDependencies,
    install : true, install_dir : get_option('bindir') / 'modules')

endif

shared_module('text.felib', 'text/TextModule.cpp', dependencies : FeatherDependency, name_prefix : '', install : true, install_dir : 'modules')
