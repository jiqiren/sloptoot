project('sloptoot', 'c',
  version : '1.3.1',
  default_options : [
    'c_std=c23',
    'warning_level=2',
    'werror=true',
  ],
)

curl_dep = dependency('libcurl', required : true)

cjson_dep = dependency('libcjson', fallback : ['cjson', 'libcjson_dep'])

sqlite_dep = dependency('sqlite3', required : true)

png_dep = dependency('libpng', required : true)
jpeg_dep = dependency('libjpeg', required : true)
nsgif_dep = dependency('libnsgif', required : true)
webp_dep = dependency('libwebp', required : true)
webpdemux_dep = dependency('libwebpdemux', required : true)

common_args = []
if host_machine.system() != 'darwin'
  common_args += ['-D_GNU_SOURCE']
endif

conf_data = configuration_data()
conf_data.set('SLOPTOOT_VERSION', meson.project_version())
version_h = configure_file(
  input : 'src/version.h.in',
  output : 'version.h',
  configuration : conf_data,
)

deps = [curl_dep, cjson_dep, sqlite_dep, png_dep, jpeg_dep, nsgif_dep,
        webp_dep, webpdemux_dep]
src = files(
  'src/main.c',
  'src/base64.c',
  'src/http.c',
  'src/json_helpers.c',
  'src/config.c',
  'src/browser.c',
  'src/loopback.c',
  'src/oauth.c',
  'src/toot.c',
  'src/cache.c',
  'src/timeline.c',
  'src/edit.c',
  'src/view.c',
  'src/image.c',
  'src/kitty.c',
)

if host_machine.system() == 'darwin'
  src += files('src/sha256_apple.c')
else
  crypto_dep = dependency('libcrypto', required : true)
  deps += [crypto_dep]
  src += files('src/sha256_posix.c')
endif

executable('sloptoot', src + version_h,
  dependencies : deps,
  c_args : common_args,
  install : true,
)

test('crypto', executable('crypto_test', 'tests/crypto_test.c',
  'src/base64.c',
  (host_machine.system() == 'darwin' ? files('src/sha256_apple.c')
                                     : files('src/sha256_posix.c')),
  include_directories : 'src',
  dependencies : (host_machine.system() == 'darwin' ? []
                                                    : dependency('libcrypto')),
  c_args : common_args,
))

test('config', executable('config_test', 'tests/config_test.c',
  'src/config.c',
  include_directories : 'src',
  c_args : common_args,
))

test('loopback', executable('loopback_test', 'tests/loopback_test.c',
  'src/loopback.c',
  include_directories : 'src',
  c_args : common_args,
))

test('cache', executable('cache_test', 'tests/cache_test.c',
  'src/cache.c',
  'src/json_helpers.c',
  include_directories : 'src',
  dependencies : [cjson_dep, sqlite_dep],
  c_args : common_args,
))

test('image', executable('image_test', 'tests/image_test.c',
  'src/image.c',
  include_directories : ['src', 'tests'],
  dependencies : [png_dep, jpeg_dep, nsgif_dep, webp_dep, webpdemux_dep],
  c_args : common_args,
))
