'use strict';

const path = require('node:path');

function finalizeConfiguration(folderPath, config, existsSync, platform = process.platform) {
  if (config.program && !path.isAbsolute(config.program) && folderPath)
    config.program = path.resolve(folderPath, config.program);

  if (config.request === 'launch' && !config.runtimeExecutable) {
    const executable = platform === 'win32' ? 'feather.exe' : 'feather';
    const candidate = folderPath && path.join(folderPath, 'build', executable);
    config.runtimeExecutable = candidate && existsSync(candidate) ? candidate : 'feather';
  }

  return config;
}

module.exports = { finalizeConfiguration };
