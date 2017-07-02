#!/usr/bin/perl -w
# (c) 2001, Dave Jones. <davej@codemonkey.org.uk> (the file handling bit)
# (c) 2005, Joel Schopp <jschopp@austin.ibm.com> (the ugly bit)
# (c) 2007, Andy Whitcroft <apw@uk.ibm.com> (new conditions, test suite, etc)
# Licensed under the terms of the GNU GPL License version 2

use strict;

my $P = $0;
$P =~ s@.*/@@g;

my $V = '0.12';

use Getopt::Long qw(:config no_auto_abbrev);

my $quiet = 0;
my $tree = 1;
my $chk_signoff = 1;
my $chk_patch = 1;
my $tst_type = 0;
my $emacs = 0;
my $terse = 0;
my $file = 0;
my $check = 0;
my $summary = 1;
my $mailback = 0;
my $root;
GetOptions(
	'q|quiet+'	=> \$quiet,
	'tree!'		=> \$tree,
	'signoff!'	=> \$chk_signoff,
	'patch!'	=> \$chk_patch,
	'test-type!'	=> \$tst_type,
	'emacs!'	=> \$emacs,
	'terse!'	=> \$terse,
	'file!'		=> \$file,
	'subjective!'	=> \$check,
	'strict!'	=> \$check,
	'root=s'	=> \$root,
	'summary!'	=> \$summary,
	'mailback!'	=> \$mailback,
) or exit;

my $exit = 0;

if ($#ARGV < 0) {
	print "usage: $P [options] patchfile\n";
	print "version: $V\n";
	print "options: -q           => quiet\n";
	print "         --no-tree    => run without a kernel tree\n";
	print "         --terse      => one line per report\n";
	print "         --emacs      => emacs compile window format\n";
	print "         --file       => check a source file\n";
	print "         --strict     => enable more subjective tests\n";
	print "         --root       => path to the kernel tree root\n";
	exit(1);
}

if ($terse) {
	$emacs = 1;
	$quiet++;
}

if ($tree) {
	if (defined $root) {
		if (!top_of_kernel_tree($root)) {
			die "$P: $root: --root does not point at a valid tree\n";
		}
	} else {
		if (top_of_kernel_tree('.')) {
			$root = '.';
		} elsif ($0 =~ m@(.*)/scripts/[^/]*$@ &&
						top_of_kernel_tree($1)) {
			$root = $1;
		}
	}

	if (!defined $root) {
		print "Must be run from the top-level dir. of a kernel tree\n";
		exit(2);
	}
}

my $emitted_corrupt = 0;

our $Ident       = qr{[A-Za-z_][A-Za-z\d_]*};
our $Storage	= qr{extern|static|asmlinkage};
our $Sparse	= qr{
			__user|
			__kernel|
			__force|
			__iomem|
			__must_check|
			__init_refok|
			__kprobes|
			fastcall
		}x;
our $Attribute	= qr{
			const|
			__read_mostly|
			__kprobes|
			__(?:mem|cpu|dev|)(?:initdata|init)
		  }x;
our $Inline	= qr{inline|__always_inline|noinline};
our $Member	= qr{->$Ident|\.$Ident|\[[^]]*\]};
our $Lval	= qr{$Ident(?:$Member)*};

our $Constant	= qr{(?:[0-9]+|0x[0-9a-fA-F]+)[UL]*};
our $Assignment	= qr{(?:\*\=|/=|%=|\+=|-=|<<=|>>=|&=|\^=|\|=|=)};
our $Operators	= qr{
			<=|>=|==|!=|
			=>|->|<<|>>|<|>|!|~|
			&&|\|\||,|\^|\+\+|--|&|\||\+|-|\*|\/
		  }x;

our $NonptrType;
our $Type;
our $Declare;

our @typeList = (
	qr{void},
	qr{char},
	qr{short},
	qr{int},
	qr{long},
	qr{unsigned},
	qr{float},
	qr{double},
	qr{bool},
	qr{long\s+int},
	qr{long\s+long},
	qr{long\s+long\s+int},
	qr{(?:__)?(?:u|s|be|le)(?:8|16|32|64)},
	qr{struct\s+$Ident},
	qr{union\s+$Ident},
	qr{enum\s+$Ident},
	qr{${Ident}_t},
	qr{${Ident}_handler},
	qr{${Ident}_handler_fn},
);

sub build_types {
	my $all = "(?:  \n" . join("|\n  ", @typeList) . "\n)";
	$NonptrType	= qr{
			\b
			(?:const\s+)?
			(?:unsigned\s+)?
			$all
			(?:\s+$Sparse|\s+const)*
			\b
		  }x;
	$Type	= qr{
			\b$NonptrType\b
			(?:\s*\*+\s*const|\s*\*+|(?:\s*\[\s*\])+)?
			(?:\s+$Sparse|\s+$Attribute)*
		  }x;
	$Declare	= qr{(?:$Storage\s+)?$Type};
}
build_types();

$chk_signoff = 0 if ($file);

my @dep_includes = ();
my @dep_functions = ();
my $removal = "Documentation/feature-removal-schedule.txt";
if ($tree && -f "$root/$removal") {
	open(REMOVE, "<$root/$removal") ||
				die "$P: $removal: open failed - $!\n";
	while (<REMOVE>) {
		if (/^Check:\s+(.*\S)/) {
			for my $entry (split(/[, ]+/, $1)) {
				if ($entry =~ m@include/(.*)@) {
					push(@dep_includes, $1);

				} elsif ($entry !~ m@/@) {
					push(@dep_functions, $entry);
				}
			}
		}
	}
}

my @rawlines = ();
for my $filename (@ARGV) {
	if ($file) {
		open(FILE, "diff -u /dev/null $filename|") ||
			die "$P: $filename: diff failed - $!\n";
	} else {
		open(FILE, "<$filename") ||
			die "$P: $filename: open failed - $!\n";
	}
	while (<FILE>) {
		chomp;
		push(@rawlines, $_);
	}
	close(FILE);
	if (!process($filename, @rawlines)) {
		$exit = 1;
	}
	@rawlines = ();
}

exit($exit);

sub top_of_kernel_tree {
	my ($root) = @_;

	my @tree_check = (
		"COPYING", "CREDITS", "Kbuild", "MAINTAINERS", "Makefile",
		"README", "Documentation", "arch", "include", "drivers",
		"fs", "init", "ipc", "kernel", "lib", "scripts",
	);

	foreach my $check (@tree_check) {
		if (! -e $root . '/' . $check) {
			return 0;
		}
	}
	return 1;
}

sub expand_tabs {
	my ($str) = @_;

	my $res = '';
	my $n = 0;
	for my $c (split(//, $str)) {
		if ($c eq "\t") {
			$res .= ' ';
			$n++;
			for (; ($n % 8) != 0; $n++) {
				$res .= ' ';
			}
			next;
		}
		$res .= $c;
		$n++;
	}

	return $res;
}
sub copy_spacing {
	my ($str) = @_;

	my $res = '';
	for my $c (split(//, $str)) {
		if ($c eq "\t") {
			$res .= $c;
		} else {
			$res .= ' ';
		}
	}

	return $res;
}

sub line_stats {
	my ($line) = @_;

	# Drop the diff line leader and expand tabs
	$line =~ s/^.//;
	$line = expand_tabs($line);

	# Pick the indent from the front of the line.
	my ($white) = ($line =~ /^(\s*)/);

	return (length($line), length($white));
}

sub sanitise_line {
	my ($line) = @_;

	my $res = '';
	my $l = '';

	my $quote = '';

	foreach my $c (split(//, $line)) {
		if ($l ne "\\" && ($c eq "'" || $c eq '"')) {
			if ($quote eq '') {
				$quote = $c;
				$res .= $c;
				$l = $c;
				next;
			} elsif ($quote eq $c) {
				$quote = '';
			}
		}
		if ($quote && $c ne "\t") {
			$res .= "X";
		} else {
			$res .= $c;
		}

		$l = $c;
	}

	return $res;
}

sub ctx_statement_block {
	my ($linenr, $remain, $off) = @_;
	my $line = $linenr - 1;
	my $blk = '';
	my $soff = $off;
	my $coff = $off - 1;

	my $type = '';
	my $level = 0;
	my $c;
	my $len = 0;
	while (1) {
		#warn "CSB: blk<$blk>\n";
		# If we are about to drop off the end, pull in more
		# context.
		if ($off >= $len) {
			for (; $remain > 0; $line++) {
				next if ($rawlines[$line] =~ /^-/);
				$remain--;
				$blk .= sanitise_line($rawlines[$line]) . "\n";
				$len = length($blk);
				$line++;
				last;
			}
			# Bail if there is no further context.
			#warn "CSB: blk<$blk> off<$off> len<$len>\n";
			if ($off == $len) {
				last;
			}
		}
		$c = substr($blk, $off, 1);

		#warn "CSB: c<$c> type<$type> level<$level>\n";
		# Statement ends at the ';' or a close '}' at the
		# outermost level.
		if ($level == 0 && $c eq ';') {
			last;
		}

		if (($type eq '' || $type eq '(') && $c eq '(') {
			$level++;
			$type = '(';
		}
		if ($type eq '(' && $c eq ')') {
			$level--;
			$type = ($level != 0)? '(' : '';

			if ($level == 0 && $coff < $soff) {
				$coff = $off;
			}
		}
		if (($type eq '' || $type eq '{') && $c eq '{') {
			$level++;
			$type = '{';
		}
		if ($type eq '{' && $c eq '}') {
			$level--;
			$type = ($level != 0)? '{' : '';

			if ($level == 0) {
				last;
			}
		}
		$off++;
	}

	my $statement = substr($blk, $soff, $off - $soff + 1);
	my $condition = substr($blk, $soff, $coff - $soff + 1);

	#warn "STATEMENT<$statement>\n";
	#warn "CONDITION<$condition>\n";

	return ($statement, $condition);
}

sub ctx_block_get {
	my ($linenr, $remain, $outer, $open, $close, $off) = @_;
	my $line;
	my $start = $linenr - 1;
	my $blk = '';
	my @o;
	my @c;
	my @res = ();

	my $level = 0;
	for ($line = $start; $remain > 0; $line++) {
		next if ($rawlines[$line] =~ /^-/);
		$remain--;

		$blk .= $rawlines[$line];
		foreach my $c (split(//, $rawlines[$line])) {
			##print "C<$c>L<$level><$open$close>O<$off>\n";
			if ($off > 0) {
				$off--;
				next;
			}

			if ($c eq $close && $level > 0) {
				$level--;
				last if ($level == 0);
			} elsif ($c eq $open) {
				$level++;
			}
		}

		if (!$outer || $level <= 1) {
			push(@res, $rawlines[$line]);
		}

		last if ($level == 0);
	}

	return ($level, @res);
}
sub ctx_block_outer {
	my ($linenr, $remain) = @_;

	my ($level, @r) = ctx_block_get($linenr, $remain, 1, '{', '}', 0);
	return @r;
}
sub ctx_block {
	my ($linenr, $remain) = @_;

	my ($level, @r) = ctx_block_get($linenr, $remain, 0, '{', '}', 0);
	return @r;
}
sub ctx_statement {
	my ($linenr, $remain, $off) = @_;

	my ($level, @r) = ctx_block_get($linenr, $remain, 0, '(', ')', $off);
	return @r;
}
sub ctx_block_level {
	my ($linenr, $remain) = @_;

	return ctx_block_get($linenr, $remain, 0, '{', '}', 0);
}
sub ctx_statement_level {
	my ($linenr, $remain, $off) = @_;

	return ctx_block_get($linenr, $remain, 0, '(', ')', $off);
}

sub ctx_locate_comment {
	my ($first_line, $end_line) = @_;

	# Catch a comment on the end of the line itself.
	my ($current_comment) = ($rawlines[$end_line - 1] =~ m@.*(/\*.*\*/)\s*$@);
	return $current_comment if (defined $current_comment);

	# Look through the context and try and figure out if there is a
	# comment.
	my $in_comment = 0;
	$current_comment = '';
	for (my $linenr = $first_line; $linenr < $end_line; $linenr++) {
		my $line = $rawlines[$linenr - 1];
		#warn "           $line\n";
		if ($linenr == $first_line and $line =~ m@^.\s*\*@) {
			$in_comment = 1;
		}
		if ($line =~ m@/\*@) {
			$in_comment = 1;
		}
		if (!$in_comment && $current_comment ne '') {
			$current_comment = '';
		}
		$current_comment .= $line . "\n" if ($in_comment);
		if ($line =~ m@\*/@) {
			$in_comment = 0;
		}
	}

	chomp($current_comment);
	return($current_comment);
}
sub ctx_has_comment {
	my ($first_line, $end_line) = @_;
	my $cmt = ctx_locate_comment($first_line, $end_line);

	##print "LINE: $rawlines[$end_line - 1 ]\n";
	##print "CMMT: $cmt\n";

	return ($cmt ne '');
}

sub cat_vet {
	my ($vet) = @_;
	my ($res, $coded);

	$res = '';
	while ($vet =~ /([^[:cntrl:]]*)([[:cntrl:]]|$)/g) {
		$res .= $1;
		if ($2 ne '') {
			$coded = sprintf("^%c", unpack('C', $2) + 64);
			$res .= $coded;
		}
	}
	$res =~ s/$/\$/;

	return $res;
}

sub annotate_values {
	my ($stream, $type) = @_;

	my $res;
	my $cur = $stream;

	my $debug = 0;

	print "$stream\n" if ($debug);

	##my $type = 'N';
	my $pos = 0;
	my $preprocessor = 0;
	my $paren = 0;
	my @paren_type;

	while (length($cur)) {
		print " <$type> " if ($debug);
		if ($cur =~ /^(\s+)/o) {
			print "WS($1)\n" if ($debug);
			if ($1 =~ /\n/ && $preprocessor) {
				$preprocessor = 0;
				$type = 'N';
			}

		} elsif ($cur =~ /^($Type)/) {
			print "DECLARE($1)\n" if ($debug);
			$type = 'T';

		} elsif ($cur =~ /^(#\s*define\s*$Ident)(\(?)/o) {
			print "DEFINE($1)\n" if ($debug);
			$preprocessor = 1;
			$paren_type[$paren] = 'N';

		} elsif ($cur =~ /^(#\s*(?:ifdef|ifndef|if|else|endif))/o) {
			print "PRE($1)\n" if ($debug);
			$preprocessor = 1;
			$type = 'N';

		} elsif ($cur =~ /^(\\\n)/o) {
			print "PRECONT($1)\n" if ($debug);

		} elsif ($cur =~ /^(sizeof)\s*(\()?/o) {
			print "SIZEOF($1)\n" if ($debug);
			if (defined $2) {
				$paren_type[$paren] = 'V';
			}
			$type = 'N';

		} elsif ($cur =~ /^(if|while|typeof|for)\b/o) {
			print "COND($1)\n" if ($debug);
			$paren_type[$paren] = 'N';
			$type = 'N';

		} elsif ($cur =~/^(return|case|else)/o) {
			print "KEYWORD($1)\n" if ($debug);
			$type = 'N';

		} elsif ($cur =~ /^(\()/o) {
			print "PAREN('$1')\n" if ($debug);
			$paren++;
			$type = 'N';

		} elsif ($cur =~ /^(\))/o) {
			$paren-- if ($paren > 0);
			if (defined $paren_type[$paren]) {
				$type = $paren_type[$paren];
				undef $paren_type[$paren];
				print "PAREN('$1') -> $type\n" if ($debug);
			} else {
				print "PAREN('$1')\n" if ($debug);
			}

		} elsif ($cur =~ /^($Ident)\(/o) {
			print "FUNC($1)\n" if ($debug);
			$paren_type[$paren] = 'V';

		} elsif ($cur =~ /^($Ident|$Constant)/o) {
			print "IDENT($1)\n" if ($debug);
			$type = 'V';

		} elsif ($cur =~ /^($Assignment)/o) {
			print "ASSIGN($1)\n" if ($debug);
			$type = 'N';

		} elsif ($cur =~ /^(;|{|}|\?|:|\[)/o) {
			print "END($1)\n" if ($debug);
			$type = 'N';

		} elsif ($cur =~ /^($Operators)/o) {
			print "OP($1)\n" if ($debug);
			if ($1 ne '++' && $1 ne '--') {
				$type = 'N';
			}

		} elsif ($cur =~ /(^.)/o) {
			print "C($1)\n" if ($debug);
		}
		if (defined $1) {
			$cur = substr($cur, length($1));
			$res .= $type x length($1);
		}
	}

	return $res;
}

sub possible {
	my ($possible) = @_;

	#print "CHECK<$possible>\n";
	if ($possible !~ /^(?:$Storage|$Type|DEFINE_\S+)$/ &&
	    $possible ne 'goto' && $possible ne 'return' &&
	    $possible ne 'struct' && $possible ne 'enum' &&
	    $possible ne 'case' && $possible ne 'else' &&
	    $possible ne 'typedef') {
		#print "POSSIBLE<$possible>\n";
		push(@typeList, $possible);
		build_types();
	}
}

my $prefix = '';

my @report = ();
sub report {
	my $line = $prefix . $_[0];

	$line = (split('\n', $line))[0] . "\n" if ($terse);

	push(@report, $line);
}
sub report_dump {
	@report;
}
sub ERROR {
	report("ERROR: $_[0]\n");
	our $clean = 0;
	our $cnt_error++;
}
sub WARN {
	report("WARNING: $_[0]\n");
	our $clean = 0;
	our $cnt_warn++;
}
sub CHK {
	if ($check) {
		report("CHECK: $_[0]\n");
		our $clean = 0;
		our $cnt_chk++;
	}
}

sub process {
	my $filename = shift;
	my @lines = @_;

	my $linenr=0;
	my $prevline="";
	my $stashline="";

	my $length;
	my $indent;
	my $previndent=0;
	my $stashindent=0;

	our $clean = 1;
	my $signoff = 0;
	my $is_patch = 0;

	our $cnt_lines = 0;
	our $cnt_error = 0;
	our $cnt_warn = 0;
	our $cnt_chk = 0;

	# Trace the real file/line as we go.
	my $realfile = '';
	my $realline = 0;
	my $realcnt = 0;
	my $here = '';
	my $in_comment = 0;
	my $first_line = 0;

	my $prev_values = 'N';

	# Pre-scan the patch looking for any __setup documentation.
	my @setup_docs = ();
	my $setup_docs = 0;
	foreach my $line (@lines) {
		if ($line=~/^\+\+\+\s+(\S+)/) {
			$setup_docs = 0;
			if ($1 =~ m@Documentation/kernel-parameters.txt$@) {
				$setup_docs = 1;
			}
			next;
		}

		if ($setup_docs && $line =~ /^\+/) {
			push(@setup_docs, $line);
		}
	}

	$prefix = '';

	foreach my $line (@lines) {
		$linenr++;

		my $rawline = $line;


#extract the filename as it passes
		if ($line=~/^\+\+\+\s+(\S+)/) {
			$realfile=$1;
			$realfile =~ s@^[^/]*/@@;
			$in_comment = 0;
			next;
		}
#extract the line range in the file after the patch is applied
		if ($line=~/^\@\@ -\d+(?:,\d+)? \+(\d+)(,(\d+))? \@\@/) {
			$is_patch = 1;
			$first_line = $linenr + 1;
			$in_comment = 0;
			$realline=$1-1;
			if (defined $2) {
				$realcnt=$3+1;
			} else {
				$realcnt=1+1;
			}
			$prev_values = 'N';
			next;
		}

# track the line number as we move through the hunk, note that
# new versions of GNU diff omit the leading space on completely
# blank context lines so we need to count that too.
		if ($line =~ /^( |\+|$)/) {
			$realline++;
			$realcnt-- if ($realcnt != 0);

			# Guestimate if this is a continuing comment.  Run
			# the context looking for a comment "edge".  If this
			# edge is a close comment then we must be in a comment
			# at context start.
			if ($linenr == $first_line) {
				my $edge;
				for (my $ln = $first_line; $ln < ($linenr + $realcnt); $ln++) {
					($edge) = ($lines[$ln - 1] =~ m@(/\*|\*/)@);
					last if (defined $edge);
				}
				if (defined $edge && $edge eq '*/') {
					$in_comment = 1;
				}
			}

			# Guestimate if this is a continuing comment.  If this
			# is the start of a diff block and this line starts
			# ' *' then it is very likely a comment.
			if ($linenr == $first_line and $line =~ m@^.\s*\*@) {
				$in_comment = 1;
			}

			# Find the last comment edge on _this_ line.
			while (($line =~ m@(/\*|\*/)@g)) {
				if ($1 eq '/*') {
					$in_comment = 1;
				} else {
					$in_comment = 0;
				}
			}

			# Measure the line length and indent.
			($length, $indent) = line_stats($line);

			# Track the previous line.
			($prevline, $stashline) = ($stashline, $line);
			($previndent, $stashindent) = ($stashindent, $indent);

		} elsif ($realcnt == 1) {
			$realcnt--;
		}

#make up the handle for any error we report on this line
		$here = "#$linenr: " if (!$file);
		$here = "#$realline: " if ($file);
		$here .= "FILE: $realfile:$realline:" if ($realcnt != 0);

		my $hereline = "$here\n$line\n";
		my $herecurr = "$here\n$line\n";
		my $hereprev = "$here\n$prevline\n$line\n";

		$prefix = "$filename:$realline: " if ($emacs && $file);
		$prefix = "$filename:$linenr: " if ($emacs && !$file);
		$cnt_lines++ if ($realcnt != 0);

#check the patch for a signoff:
		if ($line =~ /^\s*signed-off-by:/i) {
			# This is a signoff, if ugly, so do not double report.
			$signoff++;
			if (!($line =~ /^\s*Signed-off-by:/)) {
				WARN("Signed-off-by: is the preferred form\n" .
					$herecurr);
			}
			if ($line =~ /^\s*signed-off-by:\S/i) {
				WARN("need space after Signed-off-by:\n" .
					$herecurr);
			}
		}

# Check for wrappage within a valid hunk of the file
		if ($realcnt != 0 && $line !~ m{^(?:\+|-| |\\ No newline|$)}) {
			ERROR("patch seems to be corrupt (line wrapped?)\n" .
				$herecurr) if (!$emitted_corrupt++);
		}

# UTF-8 regex found at http://www.w3.org/International/questions/qa-forms-utf-8.en.php
		if (($realfile =~ /^$/ || $line =~ /^\+/) &&
		     !($line =~ m/^(
				[\x09\x0A\x0D\x20-\x7E]              # ASCII
				| [\xC2-\xDF][\x80-\xBF]             # non-overlong 2-byte
				|  \xE0[\xA0-\xBF][\x80-\xBF]        # excluding overlongs
				| [\xE1-\xEC\xEE\xEF][\x80-\xBF]{2}  # straight 3-byte
				|  \xED[\x80-\x9F][\x80-\xBF]        # excluding surrogates
				|  \xF0[\x90-\xBF][\x80-\xBF]{2}     # planes 1-3
				| [\xF1-\xF3][\x80-\xBF]{3}          # planes 4-15
				|  \xF4[\x80-\x8F][\x80-\xBF]{2}     # plane 16
				)*$/x )) {
			ERROR("Invalid UTF-8\n" . $herecurr);
		}

#ignore lines being removed
		if ($line=~/^-/) {next;}

# check we are in a valid source file if not then ignore this hunk
		next if ($realfile !~ /\.(h|c|s|S|pl|sh)$/);

#trailing whitespace
		if ($line =~ /^\+.*\015/) {
			my $herevet = "$here\n" . cat_vet($line) . "\n";
			ERROR("DOS line endings\n" . $herevet);

		} elsif ($line =~ /^\+.*\S\s+$/ || $line =~ /^\+\s+$/) {
			my $herevet = "$here\n" . cat_vet($line) . "\n";
			ERROR("trailing whitespace\n" . $herevet);
		}
#80 column limit
		if ($line =~ /^\+/ && !($prevline=~/\/\*\*/) && $length > 80) {
			WARN("line over 80 characters\n" . $herecurr);
		}

# check for adding lines without a newline.
		if ($line =~ /^\+/ && defined $lines[$linenr] && $lines[$linenr] =~ /^\\ No newline at end of file/) {
			WARN("adding a line without newline at end of file\n" . $herecurr);
		}

# check we are in a valid source file *.[hc] if not then ignore this hunk
		next if ($realfile !~ /\.[hc]$/);

# at the beginning of a line any tabs must come first and anything
# more than 8 must use tabs.
		if ($line=~/^\+\s* \t\s*\S/ or $line=~/^\+\s*        \s*/) {
			my $herevet = "$here\n" . cat_vet($line) . "\n";
			ERROR("use tabs not spaces\n" . $herevet);
		}

# Remove comments from the line before processing.
		my $comment_edge = ($line =~ s@/\*.*\*/@@g) +
				   ($line =~ s@/\*.*@@) +
				   ($line =~ s@^(.).*\*/@$1@);

# The rest of our checks refer specifically to C style
# only apply those _outside_ comments.  Only skip
# lines in the middle of comments.
		next if (!$comment_edge && $in_comment);

# Standardise the strings and chars within the input to simplify matching.
		$line = sanitise_line($line);

# Check for potential 'bare' types
		if ($realcnt &&
		    $line !~ /$Ident:\s*$/ &&
		    ($line =~ /^.\s*$Ident\s*\(\*+\s*$Ident\)\s*\(/ ||
		     $line !~ /^.\s*$Ident\s*\(/)) {
			# definitions in global scope can only start with types
			if ($line =~ /^.(?:$Storage\s+)?(?:$Inline\s+)?(?:const\s+)?($Ident)\b/) {
				possible($1);

			# declarations always start with types
			} elsif ($prev_values eq 'N' && $line =~ /^.\s*(?:$Storage\s+)?($Ident)\b\s*\**\s*$Ident\s*(?:;|=)/) {
				possible($1);

			# any (foo ... *) is a pointer cast, and foo is a type
			} elsif ($line =~ /\(($Ident)(?:\s+$Sparse)*\s*\*+\s*\)/) {
				possible($1);
			}

			# Check for any sort of function declaration.
			# int foo(something bar, other baz);
			# void (*store_gdt)(x86_descr_ptr *);
			if ($prev_values eq 'N' && $line =~ /^(.(?:(?:$Storage|$Inline)\s*)*\s*$Type\s*(?:\b$Ident|\(\*\s*$Ident\))\s*)\(/) {
				my ($name_len) = length($1);
				my ($level, @ctx) = ctx_statement_level($linenr, $realcnt, $name_len);
				my $ctx = join("\n", @ctx);

				$ctx =~ s/\n.//;
				substr($ctx, 0, $name_len + 1) = '';
				$ctx =~ s/\)[^\)]*$//;
				for my $arg (split(/\s*,\s*/, $ctx)) {
					if ($arg =~ /^(?:const\s+)?($Ident)(?:\s+$Sparse)*\s*\**\s*(:?\b$Ident)?$/ || $arg =~ /^($Ident)$/) {

						possible($1);
					}
				}
			}

		}

#
# Checks which may be anchored in the context.
#

# Check for switch () and associated case and default
# statements should be at the same indent.
		if ($line=~/\bswitch\s*\(.*\)/) {
			my $err = '';
			my $sep = '';
			my @ctx = ctx_block_outer($linenr, $realcnt);
			shift(@ctx);
			for my $ctx (@ctx) {
				my ($clen, $cindent) = line_stats($ctx);
				if ($ctx =~ /^\+\s*(case\s+|default:)/ &&
							$indent != $cindent) {
					$err .= "$sep$ctx\n";
					$sep = '';
				} else {
					$sep = "[...]\n";
				}
			}
			if ($err ne '') {
				ERROR("switch and case should be at the same indent\n$hereline$err");
			}
		}

# if/while/etc brace do not go on next line, unless defining a do while loop,
# or if that brace on the next line is for something else
		if ($line =~ /\b(?:(if|while|for|switch)\s*\(|do\b|else\b)/ && $line !~ /^.#/) {
			my ($level, @ctx) = ctx_statement_level($linenr, $realcnt, 0);
			my $ctx_ln = $linenr + $#ctx + 1;
			my $ctx_cnt = $realcnt - $#ctx - 1;
			my $ctx = join("\n", @ctx);

			# Skip over any removed lines in the context following statement.
			while ($ctx_cnt > 0 && $lines[$ctx_ln - 1] =~ /^-/) {
				$ctx_ln++;
				$ctx_cnt--;
			}
			##warn "line<$line>\nctx<$ctx>\nnext<$lines[$ctx_ln - 1]>";

			if ($ctx !~ /{\s*/ && $ctx_cnt > 0 && $lines[$ctx_ln - 1] =~ /^\+\s*{/) {
				ERROR("That open brace { should be on the previous line\n" .
					"$here\n$ctx\n$lines[$ctx_ln - 1]");
			}
			if ($level == 0 && $ctx =~ /\)\s*\;\s*$/ && defined $lines[$ctx_ln - 1]) {
				my ($nlength, $nindent) = line_stats($lines[$ctx_ln - 1]);
				if ($nindent > $indent) {
					WARN("Trailing semicolon indicates no statements, indent implies otherwise\n" .
						"$here\n$ctx\n$lines[$ctx_ln - 1]");
				}
			}
		}

		# Track the 'values' across context and added lines.
		my $opline = $line; $opline =~ s/^./ /;
		my $curr_values = annotate_values($opline . "\n", $prev_values);
		$curr_values = $prev_values . $curr_values;
		#warn "--> $opline\n";
		#warn "--> $curr_values ($prev_values)\n";
		$prev_values = substr($curr_values, -1);

#ignore lines not being added
		if ($line=~/^[^\+]/) {next;}

# TEST: allow direct testing of the type matcher.
		if ($tst_type && $line =~ /^.$Declare$/) {
			ERROR("TEST: is type $Declare\n" . $herecurr);
			next;
		}

# check for initialisation to aggregates open brace on the next line
		if ($prevline =~ /$Declare\s*$Ident\s*=\s*$/ &&
		    $line =~ /^.\s*{/) {
			ERROR("That open brace { should be on the previous line\n" . $hereprev);
		}

#
# Checks which are anchored on the added line.
#

# check for malformed paths in #include statements (uses RAW line)
		if ($rawline =~ m{^.#\s*include\s+[<"](.*)[">]}) {
			my $path = $1;
			if ($path =~ m{//}) {
				ERROR("malformed #include filename\n" .
					$herecurr);
			}
			# Sanitise this special form of string.
			$path = 'X' x length($path);
			$line =~ s{\<.*\>}{<$path>};
		}

# no C99 // comments
		if ($line =~ m{//}) {
			ERROR("do not use C99 // comments\n" . $herecurr);
		}
		# Remove C99 comments.
		$line =~ s@//.*@@;
		$opline =~ s@//.*@@;

#EXPORT_SYMBOL should immediately follow its function closing }.
		if (($line =~ /EXPORT_SYMBOL.*\((.*)\)/) ||
		    ($line =~ /EXPORT_UNUSED_SYMBOL.*\((.*)\)/)) {
			my $name = $1;
			if (($prevline !~ /^}/) &&
			   ($prevline !~ /^\+}/) &&
			   ($prevline !~ /^ }/) &&
			   ($prevline !~ /\b\Q$name\E(?:\s+$Attribute)?\s*(?:;|=)/)) {
				WARN("EXPORT_SYMBOL(foo); should immediately follow its function/variable\n" . $herecurr);
			}
		}

# check for external initialisers.
		if ($line =~ /^.$Type\s*$Ident\s*=\s*(0|NULL);/) {
			ERROR("do not initialise externals to 0 or NULL\n" .
				$herecurr);
		}
# check for static initialisers.
		if ($line =~ /\s*static\s.*=\s*(0|NULL);/) {
			ERROR("do not initialise statics to 0 or NULL\n" .
				$herecurr);
		}

# check for new typedefs, only function parameters and sparse annotations
# make sense.
		if ($line =~ /\btypedef\s/ &&
		    $line !~ /\btypedef\s+$Type\s+\(\s*\*?$Ident\s*\)\s*\(/ &&
		    $line !~ /\b__bitwise(?:__|)\b/) {
			WARN("do not add new typedefs\n" . $herecurr);
		}

# * goes on variable not on type
		if ($line =~ m{\($NonptrType(\*+)(?:\s+const)?\)}) {
			ERROR("\"(foo$1)\" should be \"(foo $1)\"\n" .
				$herecurr);

		} elsif ($line =~ m{\($NonptrType\s+(\*+)(?!\s+const)\s+\)}) {
			ERROR("\"(foo $1 )\" should be \"(foo $1)\"\n" .
				$herecurr);

		} elsif ($line =~ m{$NonptrType(\*+)(?:\s+(?:$Attribute|$Sparse))?\s+[A-Za-z\d_]+}) {
			ERROR("\"foo$1 bar\" should be \"foo $1bar\"\n" .
				$herecurr);

		} elsif ($line =~ m{$NonptrType\s+(\*+)(?!\s+(?:$Attribute|$Sparse))\s+[A-Za-z\d_]+}) {
			ERROR("\"foo $1 bar\" should be \"foo $1bar\"\n" .
				$herecurr);
		}

# # no BUG() or BUG_ON()
# 		if ($line =~ /\b(BUG|BUG_ON)\b/) {
# 			print "Try to use WARN_ON & Recovery code rather than BUG() or BUG_ON()\n";
# 			print "$herecurr";
# 			$clean = 0;
# 		}

		if ($line =~ /\bLINUX_VERSION_CODE\b/) {
			WARN("LINUX_VERSION_CODE should be avoided, code should be for the version to which it is merged" . $herecurr);
		}

# printk should use KERN_* levels.  Note that follow on printk's on the
# same line do not need a level, so we use the current block context
# to try and find and validate the current printk.  In summary the current
# printk includes all preceeding printk's which have no newline on the end.
# we assume the first bad printk is the one to report.
		if ($line =~ /\bprintk\((?!KERN_)\s*"/) {
			my $ok = 0;
			for (my $ln = $linenr - 1; $ln >= $first_line; $ln--) {
				#print "CHECK<$lines[$ln - 1]\n";
				# we have a preceeding printk if it ends
				# with "\n" ignore it, else it is to blame
				if ($lines[$ln - 1] =~ m{\bprintk\(}) {
					if ($rawlines[$ln - 1] !~ m{\\n"}) {
						$ok = 1;
					}
					last;
				}
			}
			if ($ok == 0) {
				WARN("printk() should include KERN_ facility level\n" . $herecurr);
			}
		}

# function brace can't be on same line, except for #defines of do while,
# or if closed on same line
		if (($line=~/$Type\s*[A-Za-z\d_]+\(.*\).* {/) and
		    !($line=~/\#define.*do\s{/) and !($line=~/}/)) {
			ERROR("open brace '{' following function declarations go on the next line\n" . $herecurr);
		}

# open braces for enum, union and struct go on the same line.
		if ($line =~ /^.\s*{/ &&
		    $prevline =~ /^.\s*(?:typedef\s+)?(enum|union|struct)(?:\s+$Ident)?\s*$/) {
			ERROR("open brace '{' following $1 go on the same line\n" . $hereprev);
		}

# check for spaces between functions and their parentheses.
		while ($line =~ /($Ident)\s+\(/g) {
			if ($1 !~ /^(?:if|for|while|switch|return|volatile|__volatile__|__attribute__|format|__extension__|Copyright|case)$/ &&
		            $line !~ /$Type\s+\(/ && $line !~ /^.\#\s*define\b/) {
				WARN("no space between function name and open parenthesis '('\n" . $herecurr);
			}
		}
# Check operator spacing.
		if (!($line=~/\#\s*include/)) {
			my $ops = qr{
				<<=|>>=|<=|>=|==|!=|
				\+=|-=|\*=|\/=|%=|\^=|\|=|&=|
				=>|->|<<|>>|<|>|=|!|~|
				&&|\|\||,|\^|\+\+|--|&|\||\+|-|\*|\/
			}x;
			my @elements = split(/($ops|;)/, $opline);
			my $off = 0;

			my $blank = copy_spacing($opline);

			for (my $n = 0; $n < $#elements; $n += 2) {
				$off += length($elements[$n]);

				my $a = '';
				$a = 'V' if ($elements[$n] ne '');
				$a = 'W' if ($elements[$n] =~ /\s$/);
				$a = 'B' if ($elements[$n] =~ /(\[|\()$/);
				$a = 'O' if ($elements[$n] eq '');
				$a = 'E' if ($elements[$n] eq '' && $n == 0);

				my $op = $elements[$n + 1];

				my $c = '';
				if (defined $elements[$n + 2]) {
					$c = 'V' if ($elements[$n + 2] ne '');
					$c = 'W' if ($elements[$n + 2] =~ /^\s/);
					$c = 'B' if ($elements[$n + 2] =~ /^(\)|\]|;)/);
					$c = 'O' if ($elements[$n + 2] eq '');
					$c = 'E' if ($elements[$n + 2] =~ /\s*\\$/);
				} else {
					$c = 'E';
				}

				# Pick up the preceeding and succeeding characters.
				my $ca = substr($opline, 0, $off);
				my $cc = '';
				if (length($opline) >= ($off + length($elements[$n + 1]))) {
					$cc = substr($opline, $off + length($elements[$n + 1]));
				}
				my $cb = "$ca$;$cc";

				my $ctx = "${a}x${c}";

				my $at = "(ctx:$ctx)";

				my $ptr = substr($blank, 0, $off) . "^";
				my $hereptr = "$hereline$ptr\n";

				# Classify operators into binary, unary, or
				# definitions (* only) where they have more
				# than one mode.
				my $op_type = substr($curr_values, $off + 1, 1);
				my $op_left = substr($curr_values, $off, 1);
				my $is_unary;
				if ($op_type eq 'T') {
					$is_unary = 2;
				} elsif ($op_left eq 'V') {
					$is_unary = 0;
				} else {
					$is_unary = 1;
				}
				#if ($op eq '-' || $op eq '&' || $op eq '*') {
				#	print "UNARY: <$op_left$op_type $is_unary $a:$op:$c> <$ca:$op:$cc> <$unary_ctx>\n";
				#}

				# ; should have either the end of line or a space or \ after it
				if ($op eq ';') {
					if ($ctx !~ /.x[WEB]/ && $cc !~ /^\\/ &&
					    $cc !~ /^;/) {
						ERROR("need space after that '$op' $at\n" . $hereptr);
					}

				# // is a comment
				} elsif ($op eq '//') {

				# -> should have no spaces
				} elsif ($op eq '->') {
					if ($ctx =~ /Wx.|.xW/) {
						ERROR("no spaces around that '$op' $at\n" . $hereptr);
					}

				# , must have a space on the right.
				} elsif ($op eq ',') {
					if ($ctx !~ /.xW|.xE/ && $cc !~ /^}/) {
						ERROR("need space after that '$op' $at\n" . $hereptr);
					}

				# '*' as part of a type definition -- reported already.
				} elsif ($op eq '*' && $is_unary == 2) {
					#warn "'*' is part of type\n";

				# unary operators should have a space before and
				# none after.  May be left adjacent to another
				# unary operator, or a cast
				} elsif ($op eq '!' || $op eq '~' ||
				         ($is_unary && ($op eq '*' || $op eq '-' || $op eq '&'))) {
					if ($ctx !~ /[WEB]x./ && $ca !~ /(?:\)|!|~|\*|-|\&|\||\+\+|\-\-|\{)$/) {
						ERROR("need space before that '$op' $at\n" . $hereptr);
					}
					if ($ctx =~ /.xW/) {
						ERROR("no space after that '$op' $at\n" . $hereptr);
					}

				# unary ++ and unary -- are allowed no space on one side.
				} elsif ($op eq '++' or $op eq '--') {
					if ($ctx !~ /[WOB]x[^W]/ && $ctx !~ /[^W]x[WOBE]/) {
						ERROR("need space one side of that '$op' $at\n" . $hereptr);
					}
					if ($ctx =~ /Wx./ && $cc =~ /^;/) {
						ERROR("no space before that '$op' $at\n" . $hereptr);
					}

				# << and >> may either have or not have spaces both sides
				} elsif ($op eq '<<' or $op eq '>>' or
					 $op eq '&' or $op eq '^' or $op eq '|' or
					 $op eq '+' or $op eq '-' or
					 $op eq '*' or $op eq '/')
				{
					if ($ctx !~ /VxV|WxW|VxE|WxE|VxO/) {
						ERROR("need consistent spacing around '$op' $at\n" .
							$hereptr);
					}

				# All the others need spaces both sides.
				} elsif ($ctx !~ /[EW]x[WE]/) {
					# Ignore email addresses <foo@bar>
					if (!($op eq '<' && $cb =~ /$;\S+\@\S+>/) &&
					    !($op eq '>' && $cb =~ /<\S+\@\S+$;/)) {
						ERROR("need spaces around that '$op' $at\n" . $hereptr);
					}
				}
				$off += length($elements[$n + 1]);
			}
		}

# check for multiple assignments
		if ($line =~ /^.\s*$Lval\s*=\s*$Lval\s*=(?!=)/) {
			CHK("multiple assignments should be avoided\n" . $herecurr);
		}

## # check for multiple declarations, allowing for a function declaration
## # continuation.
## 		if ($line =~ /^.\s*$Type\s+$Ident(?:\s*=[^,{]*)?\s*,\s*$Ident.*/ &&
## 		    $line !~ /^.\s*$Type\s+$Ident(?:\s*=[^,{]*)?\s*,\s*$Type\s*$Ident.*/) {
##
## 			# Remove any bracketed sections to ensure we do not
## 			# falsly report the parameters of functions.
## 			my $ln = $line;
## 			while ($ln =~ s/\([^\(\)]*\)//g) {
## 			}
## 			if ($ln =~ /,/) {
## 				WARN("declaring multiple variables together should be avoided\n" . $herecurr);
## 			}
## 		}

#need space before brace following if, while, etc
		if (($line =~ /\(.*\){/ && $line !~ /\($Type\){/) ||
		    $line =~ /do{/) {
			ERROR("need a space before the open brace '{'\n" . $herecurr);
		}

# closing brace should have a space following it when it has anything
# on the line
		if ($line =~ /}(?!(?:,|;|\)))\S/) {
			ERROR("need a space after that close brace '}'\n" . $herecurr);
		}

# check spacing on square brackets
		if ($line =~ /\[\s/ && $line !~ /\[\s*$/) {
			ERROR("no space after that open square bracket '['\n" . $herecurr);
		}
		if ($line =~ /\s\]/) {
			ERROR("no space before that close square bracket ']'\n" . $herecurr);
		}

# check spacing on paretheses
		if ($line =~ /\(\s/ && $line !~ /\(\s*(?:\\)?$/ &&
		    $line !~ /for\s*\(\s+;/) {
			ERROR("no space after that open parenthesis '('\n" . $herecurr);
		}
		if ($line =~ /\s\)/ && $line !~ /^.\s*\)/ &&
		    $line !~ /for\s*\(.*;\s+\)/) {
			ERROR("no space before that close parenthesis ')'\n" . $herecurr);
		}

#goto labels aren't indented, allow a single space however
		if ($line=~/^.\s+[A-Za-z\d_]+:(?![0-9]+)/ and
		   !($line=~/^. [A-Za-z\d_]+:/) and !($line=~/^.\s+default:/)) {
			WARN("labels should not be indented\n" . $herecurr);
		}

# Need a space before open parenthesis after if, while etc
		if ($line=~/\b(if|while|for|switch)\(/) {
			ERROR("need a space before the open parenthesis '('\n" . $herecurr);
		}

# Check for illegal assignment in if conditional.
		if ($line =~ /\bif\s*\(/) {
			my ($s, $c) = ctx_statement_block($linenr, $realcnt, 0);

			if ($c =~ /\bif\s*\(.*[^<>!=]=[^=].*/) {
				ERROR("do not use assignment in if condition ($c)\n" . $herecurr);
			}

			# Find out what is on the end of the line after the
			# conditional.
			substr($s, 0, length($c)) = '';
			$s =~ s/\n.*//g;

			if (length($c) && $s !~ /^\s*({|;|\/\*.*\*\/)?\s*\\*\s*$/) {
				ERROR("trailing statements should be on next line\n" . $herecurr);
			}
		}

# if and else should not have general statements after it
		if ($line =~ /^.\s*(?:}\s*)?else\b(.*)/ &&
		    $1 !~ /^\s*(?:\sif|{|\\|$)/) {
			ERROR("trailing statements should be on next line\n" . $herecurr);
		}

		# Check for }<nl>else {, these must be at the same
		# indent level to be relevant to each other.
		if ($prevline=~/}\s*$/ and $line=~/^.\s*else\s*/ and
						$previndent == $indent) {
			ERROR("else should follow close brace '}'\n" . $hereprev);
		}

#studly caps, commented out until figure out how to distinguish between use of existing and adding new
#		if (($line=~/[\w_][a-z\d]+[A-Z]/) and !($line=~/print/)) {
#		    print "No studly caps, use _\n";
#		    print "$herecurr";
#		    $clean = 0;
#		}

#no spaces allowed after \ in define
		if ($line=~/\#define.*\\\s$/) {
			WARN("Whitepspace after \\ makes next lines useless\n" . $herecurr);
		}

#warn if <asm/foo.h> is #included and <linux/foo.h> is available (uses RAW line)
		if ($tree && $rawline =~ m{^.\#\s*include\s*\<asm\/(.*)\.h\>}) {
			my $checkfile = "$root/include/linux/$1.h";
			if (-f $checkfile && $1 ne 'irq.h') {
				CHK("Use #include <linux/$1.h> instead of <asm/$1.h>\n" .
					$herecurr);
			}
		}

# multi-statement macros should be enclosed in a do while loop, grab the
# first statement and ensure its the whole macro if its not enclosed
# in a known goot container
		if ($prevline =~ /\#define.*\\/ &&
		   $prevline !~/(?:do\s+{|\(\{|\{)/ &&
		   $line !~ /(?:do\s+{|\(\{|\{)/ &&
		   $line !~ /^.\s*$Declare\s/) {
			# Grab the first statement, if that is the entire macro
			# its ok.  This may start either on the #define line
			# or the one below.
			my $ln = $linenr;
			my $cnt = $realcnt;
			my $off = 0;

			# If the macro starts on the define line start
			# grabbing the statement after the identifier
			$prevline =~ m{^(.#\s*define\s*$Ident(?:\([^\)]*\))?\s*)(.*)\\\s*$};
			##print "1<$1> 2<$2>\n";
			if (defined $2 && $2 ne '') {
				$off = length($1);
				$ln--;
				$cnt++;
				while ($lines[$ln - 1] =~ /^-/) {
					$ln--;
					$cnt++;
				}
			}
			my @ctx = ctx_statement($ln, $cnt, $off);
			my $ctx_ln = $ln + $#ctx + 1;
			my $ctx = join("\n", @ctx);

			# Pull in any empty extension lines.
			while ($ctx =~ /\\$/ &&
			       $lines[$ctx_ln - 1] =~ /^.\s*(?:\\)?$/) {
				$ctx .= $lines[$ctx_ln - 1];
				$ctx_ln++;
			}

			if ($ctx =~ /\\$/) {
				if ($ctx =~ /;/) {
					ERROR("Macros with multiple statements should be enclosed in a do - while loop\n" . "$here\n$ctx\n");
				} else {
					ERROR("Macros with complex values should be enclosed in parenthesis\n" . "$here\n$ctx\n");
				}
			}
		}

# check for redundant bracing round if etc
		if ($line =~ /\b(if|while|for|else)\b/) {
			# Locate the end of the opening statement.
			my @control = ctx_statement($linenr, $realcnt, 0);
			my $nr = $linenr + (scalar(@control) - 1);
			my $cnt = $realcnt - (scalar(@control) - 1);

			my $off = $realcnt - $cnt;
			#print "$off: line<$line>end<" . $lines[$nr - 1] . ">\n";

			# If this is is a braced statement group check it
			if ($lines[$nr - 1] =~ /{\s*$/) {
				my ($lvl, @block) = ctx_block_level($nr, $cnt);

				my $stmt = join("\n", @block);
				# Drop the diff line leader.
				$stmt =~ s/\n./\n/g;
				# Drop the code outside the block.
				$stmt =~ s/(^[^{]*){\s*//;
				my $before = $1;
				$stmt =~ s/\s*}([^}]*$)//;
				my $after = $1;

				#print "block<" . join(' ', @block) . "><" . scalar(@block) . ">\n";
				#print "stmt<$stmt>\n\n";

				# Count the newlines, if there is only one
				# then the block should not have {}'s.
				my @lines = ($stmt =~ /\n/g);
				#print "lines<" . scalar(@lines) . ">\n";
				if ($lvl == 0 && scalar(@lines) == 0 &&
				    $stmt !~ /{/ && $stmt !~ /\bif\b/ &&
				    $before !~ /}/ && $after !~ /{/) {
				    	my $herectx = "$here\n" . join("\n", @control, @block[1 .. $#block]) . "\n";
				    	shift(@block);
					WARN("braces {} are not necessary for single statement blocks\n" . $herectx);
				}
			}
		}

# don't include deprecated include files (uses RAW line)
		for my $inc (@dep_includes) {
			if ($rawline =~ m@\#\s*include\s*\<$inc>@) {
				ERROR("Don't use <$inc>: see Documentation/feature-removal-schedule.txt\n" . $herecurr);
			}
		}

# don't use deprecated functions
		for my $func (@dep_functions) {
			if ($line =~ /\b$func\b/) {
				ERROR("Don't use $func(): see Documentation/feature-removal-schedule.txt\n" . $herecurr);
			}
		}

# no volatiles please
		my $asm_volatile = qr{\b(__asm__|asm)\s+(__volatile__|volatile)\b};
		if ($line =~ /\bvolatile\b/ && $line !~ /$asm_volatile/) {
			WARN("Use of volatile is usually wrong: see Documentation/volatile-considered-harmful.txt\n" . $herecurr);
		}

# SPIN_LOCK_UNLOCKED & RW_LOCK_UNLOCKED are deprecated
		if ($line =~ /\b(SPIN_LOCK_UNLOCKED|RW_LOCK_UNLOCKED)/) {
			ERROR("Use of $1 is deprecated: see Documentation/spinlocks.txt\n" . $herecurr);
		}

# warn about #if 0
		if ($line =~ /^.#\s*if\s+0\b/) {
			CHK("if this code is redundant consider removing it\n" .
				$herecurr);
		}

# check for needless kfree() checks
		if ($prevline =~ /\bif\s*\(([^\)]*)\)/) {
			my $expr = $1;
			if ($line =~ /\bkfree\(\Q$expr\E\);/) {
				WARN("kfree(NULL) is safe this check is probabally not required\n" . $hereprev);
			}
		}

# warn about #ifdefs in C files
#		if ($line =~ /^.#\s*if(|n)def/ && ($realfile =~ /\.c$/)) {
#			print "#ifdef in C files should be avoided\n";
#			print "$herecurr";
#			$clean = 0;
#		}

# warn about spacing in #ifdefs
		if ($line =~ /^.#\s*(ifdef|ifndef|elif)\s\s+/) {
			ERROR("exactly one space required after that #$1\n" . $herecurr);
		}

# check for spinlock_t definitions without a comment.
		if ($line =~ /^.\s*(struct\s+mutex|spinlock_t)\s+\S+;/) {
			my $which = $1;
			if (!ctx_has_comment($first_line, $linenr)) {
				CHK("$1 definition without comment\n" . $herecurr);
			}
		}
# check for memory barriers without a comment.
		if ($line =~ /\b(mb|rmb|wmb|read_barrier_depends|smp_mb|smp_rmb|smp_wmb|smp_read_barrier_depends)\(/) {
			if (!ctx_has_comment($first_line, $linenr)) {
				CHK("memory barrier without comment\n" . $herecurr);
			}
		}
# check of hardware specific defines
		if ($line =~ m@^.#\s*if.*\b(__i386__|__powerpc64__|__sun__|__s390x__)\b@ && $realfile !~ m@include/asm-@) {
			CHK("architecture specific defines should be avoided\n" .  $herecurr);
		}

# check the location of the inline attribute, that it is between
# storage class and type.
		if ($line =~ /\b$Type\s+$Inline\b/ ||
		    $line =~ /\b$Inline\s+$Storage\b/) {
			ERROR("inline keyword should sit between storage class and type\n" . $herecurr);
		}

# Check for __inline__ and __inline, prefer inline
		if ($line =~ /\b(__inline__|__inline)\b/) {
			WARN("plain inline is preferred over $1\n" . $herecurr);
		}

# check for new externs in .c files.
		if ($line =~ /^.\s*extern\s/ && ($realfile =~ /\.c$/)) {
			WARN("externs should be avoided in .c files\n" .  $herecurr);
		}

# checks for new __setup's
		if ($rawline =~ /\b__setup\("([^"]*)"/) {
			my $name = $1;

			if (!grep(/$name/, @setup_docs)) {
				CHK("__setup appears un-documented -- check Documentation/kernel-parameters.txt\n" . $herecurr);
			}
		}

# check for pointless casting of kmalloc return
		if ($line =~ /\*\s*\)\s*k[czm]alloc\b/) {
			WARN("unnecessary cast may hide bugs, see http://c-faq.com/malloc/mallocnocast.html\n" . $herecurr);
		}
	}

	# In mailback mode only produce a report in the negative, for
	# things that appear to be patches.
	if ($mailback && ($clean == 1 || !$is_patch)) {
		exit(0);
	}

	# This is not a patch, and we are are in 'no-patch' mode so
	# just keep quiet.
	if (!$chk_patch && !$is_patch) {
		exit(0);
	}

	if (!$is_patch) {
		ERROR("Does not appear to be a unified-diff format patch\n");
	}
	if ($is_patch && $chk_signoff && $signoff == 0) {
		ERROR("Missing Signed-off-by: line(s)\n");
	}

	print report_dump();
	if ($summary) {
		print "total: $cnt_error errors, $cnt_warn warnings, " .
			(($check)? "$cnt_chk checks, " : "") .
			"$cnt_lines lines checked\n";
		print "\n" if ($quiet == 0);
	}

	if ($clean == 1 && $quiet == 0) {
		print "Your patch has no obvious style problems and is ready for submission.\n"
	}
	if ($clean == 0 && $quiet == 0) {
		print "Your patch has style problems, please review.  If any of these errors\n";
		print "are false positives report them to the maintainer, see\n";
		print "CHECKPATCH in MAINTAINERS.\n";
	}
	return $clean;
}
