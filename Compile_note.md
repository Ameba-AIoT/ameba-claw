1. 调整SDK layout，增大psram
2. 使用 `CLKCFG_1P0_AUDIO_USB`提高CPU 能效，改善刷屏速度。
3. 如若进一步提高claw 刷屏效能，可以将下列 -O3 编译选项，放在 component/ui/CMakeLists.txt。不可全局使能 -O3，否则编译报错。（ameba-claw 目录下的文件默认使能 -O3 ）

```
add_compile_options(
    "$<$<COMPILE_LANGUAGE:C>:-O3>"
    "$<$<COMPILE_LANGUAGE:CXX>:-O3>"
)
```
