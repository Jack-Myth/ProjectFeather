M1Tests = executable('m1-tests', 'M1.cpp', dependencies : FeatherDependency)
test('bytecode-vm', M1Tests)
M2Tests = executable('m2-tests', 'M2.cpp', dependencies : FeatherDependency)
test('object-dispatch', M2Tests)
M3Tests = executable('m3-tests', 'M3.cpp', dependencies : FeatherDependency)
test('gc', M3Tests)
EmbedTests = executable('embed-tests', 'Embed.cpp', dependencies : [FeatherDependency, ThreadDependency])
test('embed', EmbedTests)
M4Tests = executable('m4-tests', 'M4.cpp', dependencies : FeatherDependency)
test('source-compiler', M4Tests)
QuickTests = executable('quick-tests', 'Quick.cpp', dependencies : FeatherDependency)
test('quick', QuickTests)

test('cli-threads-source', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/threads-root.fe'],
  depends : [ThreadsNativeModule, StdioNativeModule])
ThreadArtifacts = []
foreach Name : ['threads-root', 'threads-jobs', 'threads-messages']
  ThreadArtifacts += custom_target(Name + '-fbc',
    input : 'fixtures/' + Name + '.fe', output : Name + '.fbc',
    command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default : true)
endforeach
test('cli-threads-bytecode', FeatherVmCli, args : [ThreadArtifacts[0]],
  depends : ThreadArtifacts + [ThreadsNativeModule, StdioNativeModule])
test('cli-main-error', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-main-error.fe'],
  expected_fail : true)
test('cli-quick-requires-host', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-quick.fe'],
  expected_fail : true)

# Keep the collision target in the build tree even if the protection regresses.
CompilerInputCopy = configure_file(input : 'fixtures/cli-ok.fe',
  output : 'compiler-input-copy.fe', copy : true)
test('compiler-rejects-input-as-output', FeatherCompilerCli,
  args : [CompilerInputCopy, '-o', CompilerInputCopy], expected_fail : true)

ArtifactTests = executable('artifact-tests', 'Artifact.cpp', dependencies : FeatherDependency)
test('artifact', ArtifactTests)

ImportTests = executable('import-tests', 'Import.cpp', dependencies : FeatherDependency)
test('import', ImportTests)
test('cli-multifile-source', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/multi-root.fe'], depends : [StdioNativeModule, TimeNativeModule, TextNativeModule])
test('cli-multifile-caller-relative', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/nested-root.fe'])
test('cli-multifile-cycle', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cycle-root.fe'],
  expected_fail : true)

MultiArtifacts = []
foreach Name : ['multi-root', 'math', 'util']
  MultiArtifacts += custom_target(Name + '-fbc', input : 'fixtures/' + Name + '.fe', output : Name + '.fbc',
    command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'], build_by_default : true)
endforeach
ModuleDependencies = [HelperBytecode, StdioNativeModule, TimeNativeModule, TextNativeModule]
test('cli-multifile-bytecode', FeatherVmCli, args : [MultiArtifacts[0]],
  depends : MultiArtifacts + ModuleDependencies)
test('cli-run-bytecode', FeatherCli, args : ['run', MultiArtifacts[0]],
  depends : MultiArtifacts + ModuleDependencies)

CachedSource = configure_file(
  input : 'fixtures/cache-entry-source.fe',
  output : 'cache-entry.fe',
  copy : true)
CachedBytecode = custom_target('cache-entry-fbc',
  input : 'fixtures/cache-entry-bytecode.fe', output : 'cache-entry.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true,
  build_always_stale : true)
test('cli-fresh-bytecode-cache', FeatherCli,
  args : ['run', CachedSource], depends : CachedBytecode)
MismatchedSource = configure_file(input : 'fixtures/cli-ok.fe',
  output : 'cache-size.fe', copy : true)
MismatchedBytecode = custom_target('cache-size-fbc',
  input : 'fixtures/cache-entry-source.fe', output : 'cache-size.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true, build_always_stale : true)
test('cli-bytecode-cache-size-mismatch', FeatherCli,
  args : ['run', MismatchedSource], depends : MismatchedBytecode)
test('cli-vm-rejects-debug-option', FeatherVmCli,
  args : [MultiArtifacts[0], '--wait-debugger'], depends : MultiArtifacts[0],
  expected_fail : true)

StdIoTests = executable('stdio-tests', 'StdIo.cpp', '../modules/stdio/StdIo.cpp',
  dependencies : FeatherDependency)
test('stdio', StdIoTests)
Render2DTests = executable('render2d-tests', 'Render2D.cpp',
  '../modules/render2d/Render2D.cpp',
  include_directories : include_directories('../modules/render2d'),
  dependencies : FeatherDependency)
test('render2d-core', Render2DTests)
TimeTests = executable('time-tests', 'Time.cpp',
  '../modules/time/Time.cpp',
  include_directories : include_directories('../modules/time'),
  dependencies : FeatherDependency)
test('time-core', TimeTests)
if get_option('render2d').enabled()
  test('cli-render2d-import', FeatherCli,
    args : ['run', meson.current_source_dir() / 'fixtures/cli-render2d-import.fe'],
    depends : Render2DNativeModule)
endif
ModuleLoaderTests = executable('module-loader-tests', 'ModuleLoader.cpp',
  '../src/Cli/File.cpp', '../src/Cli/ModuleLoader.cpp', '../src/Cli/NativeModules.cpp',
  cpp_args : '-DFEATHER_CLI_SOURCE_LOADER=1',
  dependencies : [FeatherDependency, NativeLoaderSystemDependency])
test('module-loader-relative-id', ModuleLoaderTests,
  args : [meson.current_source_dir() / 'fixtures/multi-root.fe'])
BudgetTests = executable('budget-tests', 'Budget.cpp', dependencies : FeatherDependency)
test('budget', BudgetTests)

DiagnosticsTests = executable('diagnostics-tests', 'Diagnostics.cpp', dependencies : FeatherDependency)
test('runtime-diagnostics', DiagnosticsTests)

if get_option('debugger')
  DebugTests = executable('debug-tests', 'Debug.cpp', dependencies : FeatherDependency)
  test('debug-protocol', DebugTests)
  DapTests = executable('dap-tests', 'Dap.cpp', dependencies : FeatherDependency)
  test('dap-adapter', DapTests)
  SocketTests = executable('socket-tests', 'Socket.cpp', '../src/Cli/Socket.cpp',
    dependencies : [FeatherDependency] + SocketSystemDependencies)
  test('debug-socket-transport', SocketTests)
endif

MultimoduleTests = executable('multimodule-tests', 'Multimodule.cpp', dependencies : FeatherDependency)
test('multimodule', MultimoduleTests)

ByteTests = executable('byte-tests', 'Byte.cpp', '../modules/stdio/StdIo.cpp',
  dependencies : FeatherDependency)
test('byte', ByteTests, args : [meson.current_build_dir() / 'byte-test.bin'])

IdleTests = executable('idle-tests', 'Idle.cpp', dependencies : FeatherDependency)
test('idle', IdleTests)

if get_option('network')
  NetworkTests = executable('network-tests', 'Network.cpp', NetworkSources,
    '../modules/stdio/StdIo.cpp', '../modules/time/Time.cpp',
    include_directories : include_directories('../modules/network'),
    dependencies : NetworkDependencies)
  test('network-loopback', NetworkTests, timeout : 30)
  test('cli-network-udp', FeatherCli,
    args : ['run', meson.current_source_dir() / 'fixtures/network-udp.fe'],
    depends : [NetworkNativeModule, TimeNativeModule])
  test('cli-network-tcp', FeatherCli,
    args : ['run', meson.current_source_dir() / 'fixtures/network-tcp.fe'],
    depends : [NetworkNativeModule, TimeNativeModule])
endif

# Requires a desktop/GPU; run this suite in graphical validation environments.
if get_option('render2d').enabled()
  Render2DDeviceTests = executable('render2d-device-tests', 'Render2DDevice.cpp',
    '../modules/render2d/Render2DBgfx.cpp',
    include_directories : Render2DModuleIncludes,
    cpp_args : Render2DModuleCppArgs,
    dependencies : Render2DModuleDependencies)
  test('render2d-device', Render2DDeviceTests,
    args : [meson.project_source_root() / 'assets/feather-logo.png',
            meson.project_source_root() / 'subprojects/bgfx-cmake/bgfx/examples/runtime/font/droidsans.ttf'],
    suite : 'graphics', is_parallel : false, timeout : 60)
endif

GetTypeTests = executable('get-type-tests', 'GetType.cpp', dependencies : FeatherDependency)
test('get-type', GetTypeTests)

AnonymousFunctionTests = executable('anonymous-function-tests', 'AnonymousFunction.cpp', dependencies : FeatherDependency)
test('anonymous-function', AnonymousFunctionTests)
test('cli-anonymous-function', FeatherCli, args : ['run', files('fixtures/anonymous-function.fe')], depends : StdioNativeModule)
AnonymousFunctionBytecode = custom_target('anonymous-function-bytecode', input : 'fixtures/anonymous-function.fe', output : 'anonymous-function.fbc', command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'], build_by_default : true)
test('cli-anonymous-function-bytecode', FeatherVmCli, args : [AnonymousFunctionBytecode], depends : [StdioNativeModule, AnonymousFunctionBytecode])

DelegateTestSource = files('fixtures/delegate.fe')
test('cli-delegate', FeatherCli, args : ['run', DelegateTestSource])
DelegateTestBytecode = custom_target('delegate-test-bytecode',
  input : DelegateTestSource, output : 'delegate-test.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'], build_by_default : true)
test('cli-delegate-bytecode', FeatherVmCli, args : [DelegateTestBytecode], depends : DelegateModuleBytecode)

if AudioEnabled
  configure_file(input : 'fixtures/audio-test.wav', output : 'audio-test.wav', copy : true)
  configure_file(input : 'fixtures/audio-test.opus', output : 'audio-test.opus', copy : true)
  AudioTestSource = files('fixtures/audio-test.fe')
  AudioTestBytecode = custom_target('audio-test-bytecode',
    input : AudioTestSource, output : 'audio-test.fbc',
    command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'], build_by_default : true)
  AudioTestEnv = {'SDL_AUDIO_DRIVER': 'dummy'}
  test('cli-audio-bytecode', FeatherVmCli, args : [AudioTestBytecode],
    env : AudioTestEnv, workdir : meson.current_build_dir(),
    depends : [AudioNativeModule, StdioNativeModule, TimeNativeModule, DelegateModuleBytecode])
endif

if is_variable('FfiNativeModule')
  FfiFixture = shared_library('ffi-fixture', 'FfiFixture.cpp', name_prefix : '', install : false)
  FfiSource = files('fixtures/ffi.fe')
  FfiBytecode = custom_target('ffi-bytecode', input : FfiSource, output : 'ffi.fbc', build_by_default : true,
    command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'])
  test('ffi-source', FeatherCli, args : ['run', FfiSource],
    workdir : meson.current_build_dir(), depends : [FfiFixture, FfiNativeModule, TextNativeModule])
  test('ffi-bytecode', FeatherVmCli, args : [FfiBytecode],
    workdir : meson.current_build_dir(), depends : [FfiFixture, FfiNativeModule, TextNativeModule])
endif
