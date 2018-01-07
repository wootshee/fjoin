## NAME
    fjoin -- fork, filter and join text stream

## SYNOPSIS
    fjoin [-c number] [-f file] [-e char] [-E] [-i char] [-I] [-o char] [-O] [-x] [--] command [args]

## DESCRIPTION
The fjoin utility distributes text records received on standard input to a number of child processes running the       specified command and merges their output back to standard output as if the input is processed by a pipeline running only a single instance of that command.

## OPTIONS
fjoin accepts the following options:

     -c      Specifies the number of child processes.

     -e      Specifies standard error record separator character (new line by default).

     -E      Exclude separators from standard error output.

     -f      Read input records from a file instead of standard input.

     -i      Specifies input separator character (new line by default).

     -I      Exclude input separator characters from sending to child processes.

     -o      Specifies output separator character (new line by default).

     -O      Exclude output separators from output.

     -x      Enable serialization of children's standard error streams.

### Separator Character Format
All options accepting the separator character must be followed either by a single character or C escape sequence string ('\n', '\000', '\xcc', etc).

## INPUT PROCESSING
fjoin treats it's input as a stream of records separated by input separator character ('\n' by default) and produces the stream of output records separated by output record separator character ('\n' by default). The output of separator characters can be suppressed.

Input records are distributed to child processes in round-robin manner. The output of child processes is joined in the same order as they receive their input. For example, if there are two child processes, the first input record is sent to a first child process, the second — to the second child, the third — to first child, the fourth — to second child, etc. The output is assembled in the same way — the first output record of the first child becomes first output record of joined output stream, the first output record of the second child becomes the second output record of joined stream, the second output record of the first child becomes the third output record, the second output record of the second child — the fourth output record, etc.

Every child process must always produce exactly one output record, including the empty one (i.e. consisting of only a separator character), for each input record. Otherwise, the behavior is undefined and may result in a deadlock.

The output of joined output stream is fully deterministic and matches the output of a pipeline with a single child process.

### Input Record Numbering
Since input records are distributed among child processes, the order in which child processes receive their input records does not match the order of input records of unforked input stream. To workaround this issue, fjoin sets FJOIN_CHILD and FJOIN_CHILDREN environment variables (refer to ENVIRONMENT section).

A child process can derive the number of input record in unforked input stream by simple formula:

     (x - 1) * FJOIN_CHILDREN + FJOIN_CHILD

where x is a record number within child process input stream.

## STANDARD ERROR PROCESSING
By default, child processes share the standard error stream with fjoin. The output of child processes sent to standard error stream is not serialized. This means that if two or more child processes write to standard error stream simultaneously, they can produce garbled output.

**-x** flag enables serialization of messages sent by child processes to their standard error streams. When this flag is enabled, fjoin expects that every child produces exactly one error record separated by separator character (specified by -e option) for every input record. If no error record is produced for input record, child process must send an empty error record consisting of a single separator character to it's standard error stream.

Separator characters can be excluded from final standard error output by using the **-E** flag.

The serialialized error stream is fully deterministic and matches the standard error stream produced by a pipeline with a single child process.

## SPECIAL CONSIDERATIONS FOR BUFFERED I/O
If child processes use buffered I/O on their standard output streams, a situation may arise when some child processes produce mostly empty or large amounts of very short output records whereas others produce many large records. In such scenario, child processes producing short records might keep the data in their output I/O buffers for relatively long periods of time. At the same time, the processes that produce long output records may need to flush their I/O buffers more frequently.

Since fjoin reads the output of it's child processes sequentially in round-robin manner, it might block on reading the output of a child process if current child process does not flush it's output buffer leading to a deadlock. To avoid it, child processes might need to flush their output buffers manually if they produce too many short records.

## ENVIRONMENT
Every child process executed by fjoin is numbered and receives it's relative number and total number of child processes in FJOIN_CHILD and FJOIN_CHILDREN environment variables.

## EXIT STATUS
The fjoin utility exits 0 on success, and >0 if an error occurs.

## EXAMPLES
Filter file myfile.txt through AWK script script.awk by 4 instances of AWK in parallel.

    fjoin -c 4 -f myfile.txt -- awk -f script.awk

Filter input lines through a slow Python script that might remove some lines from it's output using null character as output record separator. Speed up input processing by running 8 instances of Python simultaneously.

    fjoin -c 8 -O -o'\000' -- python script.py

Filter large file through 2 parallel instances of grep looking for lines containing pattern

    fjoin -c 2 -f file -- grep 'pattern'

## BUGS
fjoin cannot be used with commands that depend on the order of input records (hash generators, data compression programs, uniq, etc) or change the order of input records in their output (sort).
