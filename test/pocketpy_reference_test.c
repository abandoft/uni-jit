#include <pocketpy.h>

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    py_initialize();

    if(!py_exec("19 + 23", "<unijit-pocketpy-smoke>", EVAL_MODE, NULL)) {
        py_printexc();
        py_finalize();
        return EXIT_FAILURE;
    }
    if(!py_isint(py_retval()) || py_toint(py_retval()) != 42) {
        fputs("stock PocketPy embedding returned the wrong result\n", stderr);
        py_finalize();
        return EXIT_FAILURE;
    }

    py_finalize();
    puts("stock PocketPy 2.1.8 embedding smoke test passed");
    return EXIT_SUCCESS;
}
