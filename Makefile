#The following built-in functions are available when -mfxsr is used. All of them generate the machine instruction that is part of the name.
#
#     void __builtin_ia32_fxsave (void *)
#     void __builtin_ia32_fxrstor (void *)
#     void __builtin_ia32_fxsave64 (void *)
#     void __builtin_ia32_fxrstor64 (void *)
#The following built-in functions are available when -mxsave is used. All of them generate the machine instruction that is part of the name.
#
#     void __builtin_ia32_xsave (void *, long long)
#     void __builtin_ia32_xrstor (void *, long long)
#     void __builtin_ia32_xsave64 (void *, long long)
#     void __builtin_ia32_xrstor64 (void *, long long)
#The following built-in functions are available when -mxsaveopt is used. All of them generate the machine instruction that is part of the name.
#
#     void __builtin_ia32_xsaveopt (void *, long long)
#     void __builtin_ia32_xsaveopt64 (void *, long long)
#

CFLAGS = -Wall -Wno-format -Wno-unused -Werror -mfxsr -mxsave -mxsaveopt -static -std=gnu99
PHONY := all
all: fputest gfputest akfputest
	@:

fputest: fputest.c linux.c hexdump.c
	gcc $(CFLAGS) -Ofast -o fputest fputest.c linux.c hexdump.c

gfputest: fputest.c linux.c hexdump.c
	gcc $(CFLAGS) -g -o gfputest fputest.c linux.c hexdump.c

akfputest: fputest.c akaros.c hexdump.c
	x86_64-ucb-akaros-gcc $(CFLAGS) -Ofast -o akfputest fputest.c akaros.c hexdump.c

PHONY += clean
clean:
	rm -f fputest gfputest akfputest

.PHONY: $(PHONY)
