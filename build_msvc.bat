@echo off
rem Run from "x64 Native Tools Command Prompt for VS 2022".
rc /nologo noiseflip.rc
cl /nologo /std:c++17 /O2 /EHsc /W4 noiseflip.cpp noiseflip.res /Fe:noiseflip.exe winmm.lib ws2_32.lib shell32.lib user32.lib
cl /nologo /std:c++17 /O2 /EHsc /W4 fake_skate.cpp /Fe:fake_skate.exe
cl /nologo /std:c++17 /O1 /EHsc /W4 tests.cpp /Fe:tests.exe && tests.exe
