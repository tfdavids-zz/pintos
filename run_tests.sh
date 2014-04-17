cd src/threads/build
if [[ $1 =~ ^[0-9]+$ ]]; then
    make clean
    test=`ls ../../tests/threads/*.ck | cut -c 7- | cut -f1 -d "." | uniq | sed -n "${1}p"`
    make $test.result VERBOSE=1
elif [ "$1" != "" ]; then
    make clean
    make tests/threads/$1.result VERBOSE=1
else
    make check
fi
cd ../../..
