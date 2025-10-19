
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use locale;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Test that pg_log_query_plan() can actually log the query plan of
# a backend executing a query.

# This test requires careful timing cordination:
#  1) The target backend must be executing a query when
#     pg_log_query_plan() sends the signal.
#  2) We must confirm that the target backend actually received the
#     signal that requests logging of the plan since signals are not
#     guaranteed to be delivered instantly.
#
# We use an advisory lock to control (1) to ensure the backend is
# blocked while executing a query and an injection point to control 
# (2) to pause the plan logging handler until the target backend receive
# the signal.

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Node initialization
my $node = PostgreSQL::Test::Cluster->new('node');
$node->init();
$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

my $psql_session1 = $node->background_psql('postgres');
my $psql_session2 = $node->background_psql('postgres');

my $session1_pid = $psql_session1->query_safe("select pg_backend_pid()");

# Set injection point in the logging plan request handler to pause
# before logging.

$psql_session1->query_safe(
	qq[
	SELECT injection_points_set_local();
	SELECT injection_points_attach('log-query-interrupt', 'wait');
]);

# Use an advisory lock to make session1 blocked during query execution.
$psql_session2->query_safe(
	qq[
	BEGIN;
	SELECT pg_advisory_xact_lock(1);
]);

$psql_session1->query_until(
	qr/wait_advisory_lock/, q(
	\echo wait_advisory_lock
	BEGIN;
	SELECT pg_advisory_xact_lock(1);
));

# Confirm session1 is actually waiting on the advisory lock.
$node->wait_for_event('client backend', 'advisory');

# Now that session1 is confirmed to be executing (and blocked), request
# that the server log its query plan.
# Then commit the session 2 to release the advisory lock.
$psql_session2->query_safe(
	qq[
	SELECT pg_log_query_plan($session1_pid);
	COMMIT;
]);

# Ensure that the signal from pg_log_query_plan() is actually
# rececived by waiting on the injection point.
$node->wait_for_event('client backend', 'log-query-interrupt');

my $log_offset = -s $node->logfile;

# Detach the injection point to start logging plan.
$psql_session2->query_safe(
	qq[
    SELECT injection_points_wakeup('log-query-interrupt');
    SELECT injection_points_detach('log-query-interrupt');
]);

# Check that the plan was logged.
$node->wait_for_log('query and its plan running on backend with PID',
	$log_offset);

$psql_session1->query_safe("COMMIT;");

ok($psql_session1->quit);
ok($psql_session2->quit);

done_testing();
