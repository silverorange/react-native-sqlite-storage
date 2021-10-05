require 'json'

package = JSON.parse(File.read(File.join(__dir__, 'package.json')))

Pod::Spec.new do |s|
  s.name     = 'react-native-sqlite-storage'
  s.version  = package['version']
  s.summary  = package['description']
  s.homepage = 'https://github.com/andpor/react-native-sqlite-storage'
  s.license  = package['license']
  s.author   = package['author']
  s.source   = {
    :git => 'https://github.com/silverorange/react-native-sqlite-storage.git',
    :branch => 'search-tokenizers'
  }

  s.ios.deployment_target = '8.0'
  s.osx.deployment_target = '10.10'

  s.preserve_paths = 'README.md', 'LICENSE', 'package.json', 'sqlite.js'
  s.source_files   = 'platforms/ios/*.{h,m,c}'

  s.compiler_flags = '-DSQLITE_CORE=1'

  s.dependency 'React-Core'
  s.dependency 'Snowball', '~> 2.1.0'

  s.library = 'sqlite3'
end
