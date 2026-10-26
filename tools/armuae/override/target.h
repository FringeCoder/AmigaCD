/* armcpu's target.h: Amiberry's osdep/target.h without the front end
 * (windows, input, GUI). Only what the CPU core and its memory banks use. */
#pragma once
#include <SDL.h>
#include "options.h"
#define TARGET_NAME _T("armcpu")
#define NO_MAIN_IN_MAIN_C
#define OPTIONSFILENAME _T("default")
extern int z3_base_adr;
