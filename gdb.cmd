set pagination off
set print pretty on
file build/multi-emulator.exe
run example-cores/parallel_n64_libretro.dll "example-roms/Super Mario 64 (USA).z64" --no-audio
echo \n---- CRASH BACKTRACE ----\n
bt 30
echo \n---- REGISTERS ----\n
info registers rip rsp rbp rax rbx rcx rdx rsi rdi
echo \n---- DISASM AT FAULT ----\n
x/8i $rip
echo \n---- STACK ----\n
x/16gx $rsp
quit
