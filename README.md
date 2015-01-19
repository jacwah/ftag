Tag your files
======================

`ftag` is a command line tool to tag files. Strings are associated to
file names in a database file called `.ftagdb`. Files can later be
searched for by tag names. Unix-like systems are supported (eg. OS X
and Linux).

How to compile
-------------

Run `make` in the sources directory. The only dependency is on
SQLite 3 (http://www.sqlite.org/).

How to run
----------

Below is a short description of the three modes available and their
use. A more complete reference can be found using the --help option.

* `ftag file FILE TAG...`: Add any number of tags to the file FILE.
* `ftag filter TAG...`: Print all files tagged with one or more of
   the given tags to stdout.
* `ftag list FILE`: Print all tags associated with the file FILE on
   stdout.

Important to note is the location of the database file (`.ftagdb`). 
When the application is run it will search for this file starting
from the current directory ascending towards the root directory. If
no database file is found it is created in the current directory.
Which database file to use can also be specified with the -d,
--database-name and -p, --database-dir options.

Contact
-------

Reach me through email at jacob.wahlgren@gmail.com or in this
project's github repository at https://github.com/jacwah/ftag. You
are welcome to send pull requests or patches.
