
# Copyright (c) 2021-2023, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('ecpg');
program_version_ok('ecpg');
program_options_handling_ok('ecpg');

done_testing();
