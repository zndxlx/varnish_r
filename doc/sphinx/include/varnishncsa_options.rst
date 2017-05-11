-a

	When writing output to a file, append to it rather than overwrite it.

-b

	Log backend requests. If -c is not specified, then only backend requests will trigger log lines.

-c

	Log client requests. This is the default. If -b is specified, then -c is needed to also log client requests

-C

	Do all regular expression and string matching caseless.

-d

	Process log records at the head of the log and exit.

-D

	Daemonize.

-F <format>

	Set the output log format string.

-f <formatfile>

	Read output format from a file. Will read a single line from the specified file, and use that line as the format.

-g <request|vxid>

	The grouping of the log records. The default is to group by vxid.

-h

	Print program usage and exit

-L <limit>

	Sets the upper limit of incomplete transactions kept before the oldest transaction is force completed. A warning record is synthesized when this happens. This setting keeps an upper bound on the memory usage of running queries. Defaults to 1000 transactions.

-n <dir>

	Specify the varnishd working directory (also known as instance name) to get logs from. If -n is not specified, the host name is used.

-N <filename>

	Specify the filename of a stale VSM instance. When using this option the abandonment checking is disabled.

-P <file>

	Write the process' PID to the specified file.

-q <query>

	Specifies the VSL query to use.

-r <filename>

	Read log in binary file format from this file. The file can be created with ``varnishlog -w filename``.

-t <seconds|off>

	Timeout before returning error on initial VSM connection. If set the VSM connection is retried every 0.5 seconds for this many seconds. If zero the connection is attempted only once and will fail immediately if unsuccessful. If set to "off", the connection will not fail, allowing the utility to start and wait indefinetely for the Varnish instance to appear.  Defaults to 5 seconds.

-V

	Print version information and exit.

-w <filename>

	Redirect output to file. The file will be overwritten unless the -a option was specified. If the application receives a SIGHUP in daemon mode the file will be reopened allowing the old one to be rotated away. This option is required when running in daemon mode.

