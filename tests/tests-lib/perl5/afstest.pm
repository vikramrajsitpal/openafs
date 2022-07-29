# Copyright (c) 2020 Sine Nomine Associates. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
# IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
# NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

package afstest;
require Exporter;
our @ISA = qw(Exporter);
our @EXPORT_OK = qw(src_path obj_path is_any);

use Test::More;

sub
x_path($;$)
{
    my ($env_var, $path) = @_;
    my $tdir = $ENV{$env_var};
    if (!defined($tdir)) {
	# If C_TAP_SOURCE/C_TAP_BUILD isn't set, we assume we're running from
	# the same cwd as one of the test programs (e.g. 'tests/foo/'). So to
	# get to 'tests/', just go up one level.
	$tdir = "..";
    }

    # $tdir now represents the 'tests/' dir. Go one level up to get to the
    # top-level dir.
    if (defined($path)) {
	return "$tdir/../$path";
    } else {
	return "$tdir/..";
    }
}

sub
src_path(;$)
{
    my $path = $_[0];
    return x_path("C_TAP_SOURCE", $path);
}

sub
obj_path(;$)
{
    my $path = $_[0];
    return x_path("C_TAP_BUILD", $path);
}

# Call like:
# is_any($val, ["foo", "bar", "baz"], "is val a foobarbaz?");
# to test if $val is any of the given values
sub
is_any($$$)
{
    my ($actual, $expected, $name) = @_;

    for my $exp (@$expected) {
	if ($actual eq $exp) {
	    return ok(1, $name);
	}
    }
    return is($actual, $expected->[0], $name);
}
