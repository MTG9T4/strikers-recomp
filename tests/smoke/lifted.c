/* Stands in for lifted game code: built by clang 9 into a DLL with no C runtime, like ssbu-recomp's RECOMP_NOCRT build. */
__declspec(dllexport) int lifted_add(int a, int b) { return a + b; }
