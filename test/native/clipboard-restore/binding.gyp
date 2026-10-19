{
  "targets": [
    {
      "target_name": "clipboard-restore",
      "win_delay_load_hook": "false",
      "type": "executable",
      "sources": [
        "clipboard-restore.cc"
      ],
      "include_dirs": [
        "."
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "AdditionalOptions": [
            "/std:c++17",
            "/EHsc"
          ],
          "WholeProgramOptimization": "false"
        },
        "VCLinkerTool": {
          "GenerateDebugInformation": "false",
          "LinkTimeCodeGeneration": 0
        }
      }
    },
    {
      "target_name": "clipboard-activity",
      "win_delay_load_hook": "false",
      "type": "executable",
      "sources": [
        "../clipboard-activity.cc"
      ],
      "include_dirs": [
        "."
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "AdditionalOptions": [
            "/std:c++17",
            "/EHsc"
          ],
          "WholeProgramOptimization": "false"
        },
        "VCLinkerTool": {
          "GenerateDebugInformation": "false",
          "LinkTimeCodeGeneration": 0
        }
      }
    }
  ]
}
