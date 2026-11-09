// cxxtest: checks that C++ works: classes, virtual calls, new/delete, templates, lambdas, static constructors.
#include "pthread.h"

static int fails;
static void check(bool c, const char *what)
{
    printf(c ? "  ok   %s\n" : "  FAIL %s\n", what);
    if (!c) fails++;
}

struct Shape {
    virtual ~Shape() {}
    virtual int area() const = 0;
};
struct Rect : Shape {
    int w, h;
    Rect(int w, int h) : w(w), h(h) {}
    int area() const override { return w * h; }
};
struct Square : Rect {
    explicit Square(int s) : Rect(s, s) {}
};

template <typename T> struct Vec {
    T *data = nullptr;
    int n = 0, cap = 0;
    ~Vec() { delete[] data; }
    void push(const T &v)
    {
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            T *d = new T[cap];
            for (int i = 0; i < n; i++) d[i] = data[i];
            delete[] data;
            data = d;
        }
        data[n++] = v;
    }
    template <typename F> T fold(T acc, F f) const { for (int i = 0; i < n; i++) acc = f(acc, data[i]); return acc; }
};

static int ctor_ran;
struct Global { Global() { ctor_ran = 1; } };
static Global global;

static int &counter() { static int c = 100; return c; }

int main()
{
    printf("cxxtest\n");
    check(ctor_ran == 1, "global constructors run before main");
    Shape *s = new Square(7);
    check(s->area() == 49, "virtual calls and new");
    delete s;
    Vec<int> v;
    for (int i = 1; i <= 100; i++) v.push(i);
    check(v.fold(0, [](int a, int b) { return a + b; }) == 5050, "templates and lambdas");
    counter()++;
    check(counter() == 101, "function-local statics");
    printf(fails ? "cxxtest: %d FAILED\n" : "cxxtest: all passed\n", fails);
    return fails != 0;
}
