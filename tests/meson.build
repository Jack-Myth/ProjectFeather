M1Tests = executable('m1-tests', 'M1.cpp', dependencies : FeatherDependency)
test('m1', M1Tests)
M2Tests = executable('m2-tests', 'M2.cpp', dependencies : FeatherDependency)
test('m2', M2Tests)
M3Tests = executable('m3-tests', 'M3.cpp', dependencies : FeatherDependency)
test('m3', M3Tests)
EmbedTests = executable('embed-tests', 'Embed.cpp', dependencies : FeatherDependency)
test('embed', EmbedTests)
M4Tests = executable('m4-tests', 'M4.cpp', dependencies : FeatherDependency)
test('m4', M4Tests)
M5Tests = executable('m5-tests', 'M5.cpp', dependencies : FeatherDependency)
test('m5', M5Tests)
QuickTests = executable('quick-tests', 'Quick.cpp', dependencies : FeatherDependency)
test('quick', QuickTests)

test('cli-run', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-ok.fe'])
test('cli-main-error', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-main-error.fe'],
  expected_fail : true)
test('cli-quick-requires-host', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-quick.fe'],
  expected_fail : true)

ArtifactTests = executable('artifact-tests', 'Artifact.cpp', dependencies : FeatherDependency)
test('artifact', ArtifactTests)

ImportTests = executable('import-tests', 'Import.cpp', dependencies : FeatherDependency)
test('import', ImportTests)
test('cli-import-missing-module', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-import.fe'],
  expected_fail : true)
test('cli-multifile-source', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/multi-root.fe'])
test('cli-multifile-caller-relative', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/nested-root.fe'])
test('cli-multifile-cycle', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cycle-root.fe'],
  expected_fail : true)

MultiRootFbc = custom_target('multi-root-fbc',
  input : 'fixtures/multi-root.fe', output : 'multi-root.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true)
MathFbc = custom_target('math-fbc',
  input : 'fixtures/math.fe', output : 'math.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true)
UtilFbc = custom_target('util-fbc',
  input : 'fixtures/util.fe', output : 'util.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true)
test('cli-multifile-bytecode', FeatherVmCli,
  args : [MultiRootFbc], depends : [MathFbc, UtilFbc])
test('cli-run-bytecode', FeatherCli,
  args : ['run', MultiRootFbc], depends : [MathFbc, UtilFbc])

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
test('cli-vm-rejects-debug-option', FeatherVmCli,
  args : [MultiRootFbc, '--wait-debugger'], depends : MultiRootFbc,
  expected_fail : true)

StdIoTests = executable('stdio-tests', 'StdIo.cpp', '../stdlib/StdIo.cpp',
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
test('cli-time-import', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-time-import.fe'],
  depends : TimeNativeModule)
if get_option('render2d').enabled()
  test('cli-render2d-import', FeatherCli,
    args : ['run', meson.current_source_dir() / 'fixtures/cli-render2d-import.fe'],
    depends : Render2DNativeModule)
endif
test('cli-stdio', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-stdio.fe'],
  depends : StdioNativeModule)
StdioBytecode = custom_target('stdio-bytecode',
  input : 'fixtures/cli-stdio.fe', output : 'cli-stdio.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true)
test('cli-stdio-bytecode', FeatherVmCli,
  args : [StdioBytecode], depends : StdioNativeModule)

test('cli-snapshot-source', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-snapshot-source.fe'],
  depends : [SnapshotNativeModule, StdioNativeModule])
SnapshotBytecode = custom_target('snapshot-bytecode',
  input : 'fixtures/cli-snapshot-bytecode.fe', output : 'cli-snapshot-bytecode.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true)
test('cli-snapshot-bytecode', FeatherVmCli,
  args : [SnapshotBytecode],
  depends : [SnapshotNativeModule, StdioNativeModule, SnapshotBytecode, HelperBytecode])
test('cli-snapshot-missing', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-snapshot-missing.fe'],
  depends : SnapshotNativeModule,
  expected_fail : true)
ModuleLoaderTests = executable('module-loader-tests', 'ModuleLoader.cpp',
  '../src/Cli/File.cpp', '../src/Cli/ModuleLoader.cpp', '../src/Cli/NativeModules.cpp',
  cpp_args : '-DFEATHER_CLI_SOURCE_LOADER=1',
  dependencies : [FeatherDependency, NativeLoaderSystemDependency])
test('module-loader-relative-id', ModuleLoaderTests,
  args : [meson.current_source_dir() / 'fixtures/cli-bare.fe'])
test('cli-bare-source', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-bare.fe'])
BareBytecode = custom_target('bare-bytecode',
  input : 'fixtures/cli-bare.fe', output : 'cli-bare.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true)
test('cli-bare-bytecode', FeatherVmCli,
  args : [BareBytecode], depends : HelperBytecode)
test('cli-stdio-direct-requires-import', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-stdio-direct.fe'],
  expected_fail : true)

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
