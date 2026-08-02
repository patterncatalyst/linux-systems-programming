// Deliberately misformatted (ch46 clang-format gate). NOT part of the
// build -- CMakeLists.txt never lists this file. `clang-format --dry-run
// --Werror` against this file must exit nonzero, in contrast to the tracked
// sources in ../src, which must exit 0.
#include <cstdio>
int    add(int a,int b){
      return a+b;
}

int main(int argc,char**argv)
{
    int x=1;
        int y  =2;
    printf("%d\n",add(x,y));
  return 0 ;
}
