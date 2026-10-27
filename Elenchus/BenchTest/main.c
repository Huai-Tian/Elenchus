#include <windows.h>
#include <stdio.h>

typedef struct { HANDLE mine, theirs; long long n; } CTX_ARG;

static DWORD WINAPI Worker(LPVOID p)
{
    CTX_ARG* a = (CTX_ARG*)p;
    for (long long i = 0; i < a->n; i++)
    {
        SetEvent(a->theirs);
        WaitForSingleObject(a->mine, INFINITE);
    }
    return 0;
}

int main(int argc, char** argv)
{
    long long n = (argc > 1) ? atoll(argv[1]) : 100;
    HANDLE eA = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE eB = CreateEventA(NULL, FALSE, FALSE, NULL);
    CTX_ARG arg = { eA, eB, n };
    HANDLE th = CreateThread(NULL, 0, Worker, &arg, 0, NULL);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadAffinityMask(th, (DWORD_PTR)1 << 1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Sleep(50);
    QueryPerformanceCounter(&t0);
    for (long long i = 0; i < n; i++)
    {
        SetEvent(eA);
        WaitForSingleObject(eB, INFINITE);
    }
    QueryPerformanceCounter(&t1);
    WaitForSingleObject(th, INFINITE);
    double sec = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    printf("mini: %lld iters %.3fs -> %.0f ns/switch\n", n, sec, sec * 1e9 / (n * 2));
    return 0;
}