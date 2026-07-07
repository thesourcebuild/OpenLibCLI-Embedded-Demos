################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/OpenLibCLI/cli.c 

OBJS += \
./Core/Src/OpenLibCLI/cli.o 

C_DEPS += \
./Core/Src/OpenLibCLI/cli.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/OpenLibCLI/%.o Core/Src/OpenLibCLI/%.su Core/Src/OpenLibCLI/%.cyclo: ../Core/Src/OpenLibCLI/%.c Core/Src/OpenLibCLI/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F103xB -c -I../Core/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F1xx/Include -I../Drivers/CMSIS/Include -I"I:/openlibcli-project/OpenLibCLI-Embedded-Demos/STM32/serial/Core/Src/OpenLibCLI" -I"I:/openlibcli-project/OpenLibCLI-Embedded-Demos/STM32/serial/Core/Src/OpenLibCLI/utils" -I"I:/openlibcli-project/OpenLibCLI-Embedded-Demos/STM32/serial/Core/Src/OpenLibCLI/config" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Core-2f-Src-2f-OpenLibCLI

clean-Core-2f-Src-2f-OpenLibCLI:
	-$(RM) ./Core/Src/OpenLibCLI/cli.cyclo ./Core/Src/OpenLibCLI/cli.d ./Core/Src/OpenLibCLI/cli.o ./Core/Src/OpenLibCLI/cli.su

.PHONY: clean-Core-2f-Src-2f-OpenLibCLI

