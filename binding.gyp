{
  "targets": [
    {
      "target_name": "selection-hook",
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": [ 
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        # Use NAPI version 8, supports Node.js 18 and above, Electron 23 and above
        "NAPI_VERSION=8" 
      ],
      "conditions": [
        ['OS=="win"', {
          "sources": [ 
            "src/windows/selection_hook.cc",
            "src/windows/lib/string_pool.cc",
            "src/windows/lib/utils.cc",
            "src/windows/lib/clipboard.cc",
            "src/windows/lib/keyboard.cc"
          ],
          "libraries": [ 
            "uiautomationcore.lib",
            # Required for mouse hooks
            "user32.lib"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              # Enable C++ exception handling (/EHsc)
              "ExceptionHandling": "1" 
            }
          }
        }],
        ['OS=="mac"', {
          "sources": [ 
            "src/mac/selection_hook.mm",
            "src/mac/lib/utils.mm",
            "src/mac/lib/clipboard.mm",
            "src/mac/lib/keyboard.mm"
          ],
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "CLANG_CXX_LIBRARY": "libc++",
            "MACOSX_DEPLOYMENT_TARGET": "10.14",
            "GCC_ENABLE_CPP_RTTI": "YES"
          },
          "link_settings": {
            "libraries": [
              "-framework ApplicationServices",
              "-framework Cocoa"
            ]
          }
        }],
        ['OS=="linux"', {
          "sources": [ 
            "src/linux/selection_hook.cc",
            "src/linux/protocols/x11.cc",
            "src/linux/protocols/wayland.cc"
          ],
          "libraries": [ 
            "-levdev",
            "-lX11",
            "-lXext",
            "-lXtst",
            "-lXi"
          ],
          "cflags_cc": [
            "-std=c++17",
            "-fexceptions"
          ],
          "include_dirs": [
            "/usr/include/libevdev-1.0"
          ]
        }],
        ['OS!="win" and OS!="mac" and OS!="linux"', {
          "defines": [
            "UNSUPPORTED_PLATFORM"
          ]
        }]
      ]
    }
  ]
} 