
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use locale;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# 概要, motivation

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

#$psql_session2->query_until(
#	qr/hold_advisory_lock/, q(
#	\echo hold_advisory_lock
#	BEGIN;
#	SELECT pg_advisory_xact_lock(1);
#));

$psql_session2->query_safe(
	q(
	\echo hold_advisory_lock
	BEGIN;
	SELECT pg_advisory_xact_lock(1);
));

#$psql_session1->query_safe("BEGIN;");

$psql_session1->query_until(
	qr/wait_advisory_lock/, q(
	\echo wait_advisory_lock
	BEGIN;
	SELECT pg_advisory_xact_lock(1);
));

# Wait until the session1 is waiting on the advisory lock.
$node->wait_for_event('client backend', 'advisory');

#$psql_session2->query_until(
#	qr/log_query_plan/, qq(
#	\\echo log_query_plan
#	SELECT pg_log_query_plan($session1_pid);
#	COMMIT;
#));
$psql_session2->query_safe(
	qq[
	SELECT pg_log_query_plan($session1_pid);
	COMMIT;
]);


#$psql_session2->query_safe("COMMIT;");
#$psql_session2->query_until(
#	qr/commit/, q(
#	\\echo commit
#	commit;
#));

# Wait until the session1 is waiting on the injection point
$node->wait_for_event('client backend', 'log-query-interrupt');

my $log_offset = -s $node->logfile;

# Detach injection point.
$psql_session2->query_safe(
	qq[
    SELECT injection_points_wakeup('log-query-interrupt');
    SELECT injection_points_detach('log-query-interrupt');
]);


# Check that the plan was logged.
$node->wait_for_log('query and its plan running on backend with PID',
	$log_offset);

$psql_session1->query_safe("COMMIT;");

# Continue the paused session.

ok($psql_session1->quit);
ok($psql_session2->quit);
done_testing();
