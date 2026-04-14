@echo off

pushd ..\

mkdir Bin

pushd Bin

set CONFIG="release"

if %CONFIG% equ "debug" (
    set CL=/Od /Z7 /RTC1 /MTd /fsanitize=address
    rem set CL=/Od /Z7 /RTC1 /MTd
    set LINK=/DEBUG
) else (
    set CL=/GL /O2 /MT /DNDEBUG /GS-
    set LINK=/NOCOFFGRPINFO /EMITTOOLVERSIONINFO:NO /LTCG /OPT:REF /OPT:ICF
)

@echo on
rem IS /EHsc avoidable?
cl /nologo ..\Source\Win32Main.cpp /I..\Source /Fe"TonalBalanceAnalyzer.exe" /W4 /WX /EHsc /link /INCREMENTAL:NO
@echo off

popd
popd