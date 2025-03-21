#!/usr/bin/perl
#
# Generate a composition table and its lookup utilities, using Unicode data
# files as input.
#
# Input: UnicodeData.txt and CompositionExclusions.txt
# Output: unicode_norm_table.h and unicode_norm_hashfunc.h
#
# Copyright (c) 2000-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

use FindBin;
use lib "$FindBin::RealBin/../../tools/";
use PerfectHash;

my $output_path = '.';

GetOptions('outdir:s' => \$output_path);

my $output_table_file = "$output_path/unicode_norm_table.h";
my $output_func_file = "$output_path/unicode_norm_hashfunc.h";


my $FH;

# Read list of codes that should be excluded from re-composition.
my @composition_exclusion_codes = ();
open($FH, '<', "$output_path/CompositionExclusions.txt")
  or die "Could not open $output_path/CompositionExclusions.txt: $!.";
while (my $line = <$FH>)
{
	if ($line =~ /^([[:xdigit:]]+)/)
	{
		push @composition_exclusion_codes, $1;
	}
}
close $FH;

# Read entries from UnicodeData.txt into a list, and a hash table. We need
# three fields from each row: the codepoint, canonical combining class,
# and character decomposition mapping
my @characters = ();
my %character_hash = ();
open($FH, '<', "$output_path/UnicodeData.txt")
  or die "Could not open $output_path/UnicodeData.txt: $!.";
while (my $line = <$FH>)
{

	# Split the line wanted and get the fields needed:
	# - Unicode code value
	# - Canonical Combining Class
	# - Character Decomposition Mapping
	my @elts = split(';', $line);
	my $code = $elts[0];
	my $class = $elts[3];
	my $decomp = $elts[5];

	# Skip codepoints above U+10FFFF. They cannot be represented in 4 bytes
	# in UTF-8, and PostgreSQL doesn't support UTF-8 characters longer than
	# 4 bytes. (This is just pro forma, as there aren't any such entries in
	# the data file, currently.)
	next if hex($code) > 0x10FFFF;

	# Skip characters with no decompositions and a class of 0, to reduce the
	# table size.
	next if $class eq '0' && $decomp eq '';

	my %char_entry = (code => $code, class => $class, decomp => $decomp);
	push(@characters, \%char_entry);
	$character_hash{$code} = \%char_entry;
}
close $FH;

my $num_characters = scalar @characters;

# Start writing out the output files
open my $OT, '>', $output_table_file
  or die "Could not open output file $output_table_file: $!\n";
open my $OF, '>', $output_func_file
  or die "Could not open output file $output_func_file: $!\n";

print $OT <<HEADER;
/*-------------------------------------------------------------------------
 *
 * unicode_norm_table.h
 *	  Composition table used for Unicode normalization
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/unicode_norm_table.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * File auto-generated by src/common/unicode/generate-unicode_norm_table.pl,
 * do not edit. There is deliberately not an #ifndef PG_UNICODE_NORM_TABLE_H
 * here.
 */
typedef struct
{
	uint32		codepoint;		/* Unicode codepoint */
	uint8		comb_class;		/* combining class of character */
	uint8		dec_size_flags; /* size and flags of decomposition code list */
	uint16		dec_index;		/* index into UnicodeDecomp_codepoints, or the
								 * decomposition itself if DECOMP_INLINE */
} pg_unicode_decomposition;

#define DECOMP_NO_COMPOSE	0x80	/* don't use for re-composition */
#define DECOMP_INLINE		0x40	/* decomposition is stored inline in
									 * dec_index */
#define DECOMP_COMPAT		0x20	/* compatibility mapping */

#define DECOMPOSITION_SIZE(x) ((x)->dec_size_flags & 0x1F)
#define DECOMPOSITION_NO_COMPOSE(x) (((x)->dec_size_flags & (DECOMP_NO_COMPOSE | DECOMP_COMPAT)) != 0)
#define DECOMPOSITION_IS_INLINE(x) (((x)->dec_size_flags & DECOMP_INLINE) != 0)
#define DECOMPOSITION_IS_COMPAT(x) (((x)->dec_size_flags & DECOMP_COMPAT) != 0)

/* Table of Unicode codepoints and their decompositions */
static const pg_unicode_decomposition UnicodeDecompMain[$num_characters] =
{
HEADER

print $OF <<HEADER;
/*-------------------------------------------------------------------------
 *
 * unicode_norm_hashfunc.h
 *	  Perfect hash functions used for Unicode normalization
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/unicode_norm_hashfunc.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * File auto-generated by src/common/unicode/generate-unicode_norm_table.pl,
 * do not edit. There is deliberately not an #ifndef PG_UNICODE_NORM_HASHFUNC_H
 * here.
 */

#include "common/unicode_norm_table.h"

/* Typedef for perfect hash functions */
typedef int (*cp_hash_func) (const void *key);

/* Information for lookups with perfect hash functions */
typedef struct
{
	const pg_unicode_decomposition *decomps;
	cp_hash_func	hash;
	int		num_decomps;
} pg_unicode_decompinfo;

typedef struct
{
	const uint16	*inverse_lookup;
	cp_hash_func	hash;
	int		num_recomps;
} pg_unicode_recompinfo;

HEADER

my $decomp_index = 0;
my $decomp_string = "";
my @dec_cp_packed;
my $main_index = 0;
my @rec_info;

my $last_code = $characters[-1]->{code};
foreach my $char (@characters)
{
	my $code = $char->{code};
	my $class = $char->{class};
	my $decomp = $char->{decomp};

	# Save the code point bytes as a string in network order.
	push @dec_cp_packed, pack('N', hex($char->{code}));

	# The character decomposition mapping field in UnicodeData.txt is a list
	# of unicode codepoints, separated by space. But it can be prefixed with
	# so-called compatibility formatting tag, like "<compat>", or "<font>".
	# The entries with compatibility formatting tags should not be used for
	# re-composing characters during normalization, so flag them in the table.
	# (The tag doesn't matter, only whether there is a tag or not)
	my $compat = 0;
	if ($decomp =~ /\<.*\>/)
	{
		$compat = 1;
		$decomp =~ s/\<[^][]*\>//g;
	}
	my @decomp_elts = split(" ", $decomp);

	# Decomposition size
	# Print size of decomposition
	my $decomp_size = scalar(@decomp_elts);
	die if $decomp_size > 0x1F;    # to not overrun bitmask

	my $first_decomp = shift @decomp_elts;

	my $flags = "";
	my $comment = "";

	if ($compat)
	{
		$flags .= " | DECOMP_COMPAT";
	}

	if ($decomp_size == 2)
	{
		# Should this be used for recomposition?
		if (   $character_hash{$first_decomp}
			&& $character_hash{$first_decomp}->{class} != 0)
		{
			$flags .= " | DECOMP_NO_COMPOSE";
			$comment = "non-starter decomposition";
		}
		else
		{
			foreach my $lcode (@composition_exclusion_codes)
			{
				if ($lcode eq $code)
				{
					$flags .= " | DECOMP_NO_COMPOSE";
					$comment = "in exclusion list";
					last;
				}
			}
		}

		# Save info for recomposeable codepoints.
		# Note that this MUST match the macro DECOMPOSITION_NO_COMPOSE in C
		# above!  See also the inverse lookup in recompose_code() found in
		# src/common/unicode_norm.c.
		if (!($flags =~ /DECOMP_COMPAT/ || $flags =~ /DECOMP_NO_COMPOSE/))
		{
			push @rec_info,
			  {
				code => $code,
				main_index => $main_index,
				first => $first_decomp,
				second => $decomp_elts[0]
			  };
		}
	}

	if ($decomp_size == 0)
	{
		print $OT "\t{0x$code, $class, 0$flags, 0}";
	}
	elsif ($decomp_size == 1 && length($first_decomp) <= 4)
	{

		# The decomposition consists of a single codepoint, and it fits
		# in a uint16, so we can store it "inline" in the main table.
		$flags .= " | DECOMP_INLINE";
		print $OT "\t{0x$code, $class, 1$flags, 0x$first_decomp}";
	}
	else
	{
		print $OT "\t{0x$code, $class, $decomp_size$flags, $decomp_index}";

		# Now save the decompositions into a dedicated area that will
		# be written afterwards.  First build the entry dedicated to
		# a sub-table with the code and decomposition.
		$decomp_string .= ",\n" if ($decomp_string ne "");

		$decomp_string .= "\t /* $decomp_index */ 0x$first_decomp";
		foreach (@decomp_elts)
		{
			$decomp_string .= ", 0x$_";
		}

		$decomp_index = $decomp_index + $decomp_size;
	}

	# Print a comma after all items except the last one.
	print $OT "," unless ($code eq $last_code);

	print $OT "\t/* $comment */" if ($comment ne "");
	print $OT "\n";

	$main_index++;
}
print $OT "\n};\n\n";

# Print the array of decomposed codes.
print $OT <<HEADER;
/* codepoints array  */
static const uint32 UnicodeDecomp_codepoints[$decomp_index] =
{
$decomp_string
};
HEADER

# Emit the definition of the decomp hash function.
my $dec_funcname = 'Decomp_hash_func';
my $dec_func = PerfectHash::generate_hash_function(\@dec_cp_packed,
	$dec_funcname, fixed_key_length => 4);
print $OF "/* Perfect hash function for decomposition */\n";
print $OF "static $dec_func\n";

# Emit the structure that wraps the hash lookup information into
# one variable.
print $OF <<HEADER;
/* Hash lookup information for decomposition */
static const pg_unicode_decompinfo UnicodeDecompInfo =
{
	UnicodeDecompMain,
	$dec_funcname,
	$num_characters
};

HEADER

# Find the lowest codepoint that decomposes to each recomposeable
# code pair and create a mapping to it.
my $recomp_string = "";
my @rec_cp_packed;
my %seenit;
my $firstentry = 1;
foreach my $rec (sort recomp_sort @rec_info)
{
	# The hash key is formed by concatenating the bytes of the two
	# codepoints. See also recompose_code() in common/unicode_norm.c.
	my $hashkey = (hex($rec->{first}) << 32) | hex($rec->{second});

	# We are only interested in the lowest code point that decomposes
	# to the given code pair.
	next if $seenit{$hashkey};

	# Save the hash key bytes in network order
	push @rec_cp_packed, pack('Q>', $hashkey);

	# Append inverse lookup element
	$recomp_string .= ",\n" if !$firstentry;
	$recomp_string .= sprintf "\t/* U+%s+%s -> U+%s */ %s",
	  $rec->{first},
	  $rec->{second},
	  $rec->{code},
	  $rec->{main_index};

	$seenit{$hashkey} = 1;
	$firstentry = 0;
}

# Emit the inverse lookup array containing indexes into UnicodeDecompMain.
my $num_recomps = scalar @rec_cp_packed;
print $OF <<HEADER;
/* Inverse lookup array -- contains indexes into UnicodeDecompMain[] */
static const uint16 RecompInverseLookup[$num_recomps] =
{
$recomp_string
};

HEADER

# Emit the definition of the recomposition hash function.
my $rec_funcname = 'Recomp_hash_func';
my $rec_func =
  PerfectHash::generate_hash_function(\@rec_cp_packed, $rec_funcname,
	fixed_key_length => 8);
print $OF "/* Perfect hash function for recomposition */\n";
print $OF "static $rec_func\n";

# Emit the structure that wraps the hash lookup information into
# one variable.
print $OF <<HEADER;
/* Hash lookup information for recomposition */
static const pg_unicode_recompinfo UnicodeRecompInfo =
{
	RecompInverseLookup,
	$rec_funcname,
	$num_recomps
};
HEADER

close $OT;
close $OF;

sub recomp_sort
{
	my $a1 = hex($a->{first});
	my $b1 = hex($b->{first});

	my $a2 = hex($a->{second});
	my $b2 = hex($b->{second});

	# First sort by the first code point
	return -1 if $a1 < $b1;
	return 1 if $a1 > $b1;

	# Then sort by the second code point
	return -1 if $a2 < $b2;
	return 1 if $a2 > $b2;

	# Finally sort by the code point that decomposes into first and
	# second ones.
	my $acode = hex($a->{code});
	my $bcode = hex($b->{code});

	return -1 if $acode < $bcode;
	return 1 if $acode > $bcode;

	die "found duplicate entries of recomposeable code pairs";
}
