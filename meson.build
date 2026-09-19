project(
  'project-feather',
  'cpp',
  meson_version : '>=1.11.0',
  version : '0.3.1',
  default_options : [
    'cpp_std=c++20',
    'warning_level=2',
  ],
)

if host_machine.system() == 'windows'
  add_project_arguments('/wd4251', language : 'cpp')
endif

FeatherSources = files(
  'src/Value/Value.cpp',
  'src/Bytecode/Bytecode.cpp',
  'src/Bytecode/ProgramImage.cpp',
  'src/Bytecode/SymbolImage.cpp',
  'src/Vm/Vm.cpp',
  'src/Object/Object.cpp',
  'src/Compiler/Compiler.cpp',
  'src/Embed/Import.cpp',
  'src/Snapshot/Snapshot.cpp',
)

if get_option('debugger')
  FeatherSources += files(
    'src/Protocol/Json.cpp',
    'src/Debug/Debug.cpp',
    'src/Debug/Dap.cpp',
  )
endif

# Core and the optional debugger are one position-independent static runtime.
# Interpreter binaries link it whole so the debugger is really bundled;
# the compiler and native modules pull only referenced objects. No target
# requires a sibling feather-core.dll.
FeatherRuntime = static_library(
  'feather-runtime',
  FeatherSources,
  include_directories : include_directories('include'),
  pic : true,
  install : true,
)

FeatherDependency = declare_dependency(
  link_with : FeatherRuntime,
  include_directories : include_directories('include'),
)
FeatherBundledDependency = declare_dependency(
  link_whole : FeatherRuntime,
  include_directories : include_directories('include'),
)
install_subdir('include/Feather', install_dir : get_option('includedir'))

NativeLoaderSystemDependency = meson.get_compiler('cpp').find_library('dl', required : false)
SocketSystemDependencies = []
if get_option('debugger') and host_machine.system() == 'windows'
  SocketSystemDependencies += meson.get_compiler('cpp').find_library('ws2_32', required : true)
endif

FeatherCliSources = files('src/Cli/Main.cpp', 'src/Cli/File.cpp', 'src/Cli/Runner.cpp',
  'src/Cli/ModuleLoader.cpp', 'src/Cli/NativeModules.cpp', 'src/Cli/SnapshotHost.cpp')
FeatherVmCliSources = files('src/Cli/VmMain.cpp', 'src/Cli/File.cpp', 'src/Cli/Runner.cpp',
  'src/Cli/ModuleLoader.cpp', 'src/Cli/NativeModules.cpp', 'src/Cli/SnapshotHost.cpp')
FeatherCliArguments = ['-DFEATHER_CLI_SOURCE_LOADER=1']
if get_option('debugger')
  FeatherCliSources += files('src/Cli/Socket.cpp', 'src/Cli/DebugHost.cpp')
  FeatherCliArguments += '-DFEATHER_CLI_DEBUGGER=1'
endif

FeatherCli = executable('feather', FeatherCliSources,
  cpp_args : FeatherCliArguments,
  dependencies : [FeatherBundledDependency, NativeLoaderSystemDependency] + SocketSystemDependencies,
  install : true)
FeatherCompilerCli = executable('featherc', 'src/Cli/CompilerMain.cpp',
  'src/Cli/File.cpp', dependencies : FeatherDependency, install : true)
FeatherVmCli = executable('feathervm', FeatherVmCliSources,
  dependencies : [FeatherDependency, NativeLoaderSystemDependency],
  install : true)

if get_option('debugger')
  FeatherDebuggerCli = executable('feather-debugger',
    'src/Cli/DebuggerMain.cpp', 'src/Cli/Socket.cpp',
    dependencies : [FeatherDependency] + SocketSystemDependencies,
    install : true)
endif

subdir('modules')
subdir('tests')
