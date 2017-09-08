suppressPackageStartupMessages(library(optparse))

# Example data rows, one per test iteration:
# data[1:2,]
# 					V1       V2 V3
#  1 XSAVE_DIRTY_RSTOR baseline 33
#  2 XSAVE_DIRTY_RSTOR baseline 35
#
# levels(data$V1)	(these are the test types)
# [1] "RSTOR_DIRTY_XSAVE"    "RSTOR_DIRTY_XSAVEOPT" "XSAVE_DIRTY_RSTOR"   
# [4] "XSAVEOPT_DIRTY_RSTOR"
#
# levels(data$V2)	(these are the type of dirtying)
# [1] "all"      "baseline" "fpu"      "xmm"   

### build a vector like c(1,2,3,4,  7,8,9,10, ...) to space the tests
vec_spacing <- function(d) {
	spacing <- c()
	pos <- 1
	for (i in 1:length(levels(d$V1))) {
		for (j in 1:length(levels(d$V2))) {
			spacing <- append(spacing, pos)
			pos <- pos + 1
		}
		pos <- pos + 2
	}
	return(spacing)
}

# Finds the FOO after 'pattern' from a line in infile, e.g. "patternFOO".
# There's probably an R helper for this.
extract_meta_val <- function(infile, pattern)
{
	lines <- readLines(infile)
	titular_line <- grep(pattern, lines, value = TRUE)
	idx <- attr(regexpr(pattern, titular_line), "match.length") + 1
	return(substr(titular_line, idx, nchar(titular_line)))
}

get_title <- function(infile) {
	return(extract_meta_val(infile, "# title: "))
}

get_machine <- function(infile) {
	return(extract_meta_val(infile, "# machine: "))
}

### collect command line arguments
# establish optional arguments
# "-h" and "--help" are automatically in the list
option_list <- list(
	make_option(c("-i", "--input"), type = "character",
		default = "raw.dat",
		help = "Input data file"),
	make_option(c("-o", "--output"), type = "character",
		default = "graph.pdf",
		help = "Output file"),
	make_option("--ymin", type = "double", default = 0),
	make_option("--ymax", type = "double", default = -1)
)

opt <- parse_args(OptionParser(option_list=option_list))
data <- read.table(opt$input)
title <- get_title(opt$input)
machine <- get_machine(opt$input)
plot_header <- paste(title, machine, sep = '\n')

# probably better ways to do this automatically (without the opts)
ymax <- max(data$V3)
if (opt$ymax != -1) {
	ymax = min(opt$ymax, ymax)
}

pdf(opt$output)

# Hacked values for our x labels, after rotation
par(mar = c(16,4,4,2))
# Fixed-width font to easily align x axis values
par(family = "mono")

# This sorts the tests factors into the order they appear in the input
tests_f <- factor(data$V1, levels = unique(c(levels(data$V1)[data$V1])))
# This sorts the dirty factors into the order they appear in the input
dirty_f <- factor(data$V2, levels = unique(c(levels(data$V2)[data$V2])))
# lex.order = TRUE will group them by test type
factors <- interaction(tests_f, dirty_f, lex.order = TRUE)

# to do points instead of a boxplot, for boxplot, set border = "white" and then:
# points(interaction(data$V1, data$V2), data$V3)
spacing <- vec_spacing(data)
boxplot(data$V3 ~ factors, las = 2, main = plot_header, ylim = c(opt$ymin,ymax),
        at = spacing, cex.axis = 0.5, ylab = "Unhalted Core Cycles")
abline(v = spacing, lty = 3, col = "grey")
invisible(dev.off())
