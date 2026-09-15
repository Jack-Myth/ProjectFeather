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
test('artifact', ArtifactTests,
  args : [meson.current_source_dir() / 'fixtures/compat-v2.fbc'])
test('cli-compat-v2', FeatherVmCli,
  args : [meson.current_source_dir() / 'fixtures/compat-v2.fbc'])

ImportTests = executable('import-tests', 'Import.cpp', dependencies : FeatherDependency)
test('import', ImportTests)
test('cli-import-requires-host', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-import.fe'],
  expected_fail : true)

StdIoTests = executable('stdio-tests', 'StdIo.cpp',
  dependencies : FeatherStdIoDependency)
test('stdio', StdIoTests)
test('cli-stdio', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-stdio.fe'])
test('cli-stdio-direct-requires-import', FeatherCli,
  args : ['run', meson.current_source_dir() / 'fixtures/cli-stdio-direct.fe'],
  expected_fail : true)

BudgetTests = executable('budget-tests', 'Budget.cpp', dependencies : FeatherDependency)
test('budget', BudgetTests)
