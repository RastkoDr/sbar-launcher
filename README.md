# Arguments
-h {bar height}
-w {bar width}
-r retained mode (the bar stays active after the command is run and displays the output of the command)
-d dark mode 

## Notice
This project was made mostly using copilot as an experiment. It will receive minimal to no support. I would not recomend using this as it is barely tested.
Anyone is free to submit changes and additions, but the codebase is not nice to work in.

## Compiling 
Both gcc and clang tested and working
I recommend:
```bash
clang main.c -O2 -lX11 -o launcher-bar
```
