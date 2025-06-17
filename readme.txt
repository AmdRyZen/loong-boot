git filter-branch --commit-filter '
        if [ "$GIT_AUTHOR_NAME" = "xx•xx" ];
        then
                export GIT_AUTHOR_NAME="神圣•凯莎";
                export GIT_AUTHOR_EMAIL="kaisha@gmail.com";
        fi;
        git commit-tree "$@";
' --tag-name-filter cat -- --branches --tags

git push --force origin main


// 数据对齐
__attribute__((aligned(n)))

cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_MAKE_PROGRAM=/Users/shenshengkaisha/Applications/CLion.app/Contents/bin/ninja/mac/aarch64/ninja -DCMAKE_C_COMPILER=/opt/homebrew/Cellar/gcc@11/11.4.0/bin/gcc-11 -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/gcc@11/11.4.0/bin/g++-11 -G Ninja -S /Users/shenshengkaisha/Desktop/github/drogon-http -B /Users/shenshengkaisha/Desktop/github/drogon-http/build