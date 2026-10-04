
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use locale;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Test that pg_log_query_plan() actually logs the query plan of
# another backend executing a query.

# This test requires timing coordinations:
#  1) The target backend must be executing a query when
#     pg_log_query_plan() sends the signal.
#  2) We must confirm that the target backend actually received the
#     signal that requests logging of the plan.
#
# We use an advisory lock and an injection point to control them
# respectively.

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
my $psql_session3 = $node->background_psql('postgres');

my $session1_pid = $psql_session1->query_safe("select pg_backend_pid()");

# Set injection point in the logging plan request handler to ensure
# that session1 received the signal of pg_log_query_plan().
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
	qr/wait_on_advisory_lock/, q(
	\echo wait_on_advisory_lock
	BEGIN;
	SELECT pg_advisory_xact_lock(1);
));

# Confirm that session1 is actually waiting on the advisory lock.
$node->wait_for_event('client backend', 'advisory');

# Stop the sender after publishing the request but before sending the signal.
$psql_session2->query_safe(
	qq[
	SELECT injection_points_set_local();
	SELECT injection_points_attach('log-query-plan-before-signal', 'wait');
]);
$psql_session2->query_until(
	qr/sending_request/,
	"\\echo sending_request\nSELECT pg_log_query_plan($session1_pid);\n");
$node->wait_for_event('client backend', 'log-query-plan-before-signal');

my ($pending_result, $pending_error) =
  $psql_session3->query("SELECT pg_log_query_plan($session1_pid);");
is($pending_result, '',
	'a second request is rejected while the first is pending');
ok($pending_error, 'the pending request reports a busy warning');

$node->safe_psql(
	'postgres',
	qq[
	SELECT injection_points_wakeup('log-query-plan-before-signal');
	SELECT injection_points_detach('log-query-plan-before-signal');
]);
my $request_id = $psql_session2->query_safe('');
like($request_id, qr/^[1-9][0-9]*$/,
	'the first request returns a positive request ID');

# Ensure that the signal of pg_log_query_plan() is actually
# received by confirming session1 is waiting on the injection point.
$node->wait_for_event('client backend', 'log-query-interrupt');

# A target backend accepts at most one outstanding request.  Confirm that a
# second request cannot overwrite the request being processed.
my ($second_result, $second_error) =
  $psql_session3->query("SELECT pg_log_query_plan($session1_pid);");
is($second_result, '',
	'a second request is rejected while the first is being processed');
ok($second_error, 'the processing request reports a busy warning');

# Commit the session 2 to release the advisory lock.
$psql_session2->query_safe("COMMIT;");

my $log_offset = -s $node->logfile;

# Detach the injection point to start logging the plan.
$psql_session2->query_safe(
	qq[
    SELECT injection_points_wakeup('log-query-interrupt');
    SELECT injection_points_detach('log-query-interrupt');
]);

$node->wait_for_log(
	qr/request ID $request_id running on backend with PID $session1_pid /,
	$log_offset);

# The completed request must release the slot.  At this point session1 is
# idle, so the accepted request cannot produce a plan, but its failure log
# must still identify the request.
$log_offset = -s $node->logfile;
my $next_request_id =
  $psql_session2->query_safe("SELECT pg_log_query_plan($session1_pid);");
like($next_request_id, qr/^[1-9][0-9]*$/,
	'a new request returns a positive request ID after the first completes');
$node->wait_for_log(
	qr/query plan logging request $next_request_id could not be completed because there was no opportunity to log the plan/,
	$log_offset);

$psql_session1->query_safe("COMMIT;");

ok($psql_session1->quit);
ok($psql_session2->quit);
ok($psql_session3->quit);

done_testing();
