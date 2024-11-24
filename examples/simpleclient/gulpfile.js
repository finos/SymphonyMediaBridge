var gulp = require("gulp"), connect = require('gulp-connect');
var browserify = require("browserify");
var source = require("vinyl-source-stream"), buffer = require('vinyl-buffer');
var watchify = require("watchify");
var tsify = require("tsify");
var fancy_log = require("fancy-log");
var sourcemaps = require('gulp-sourcemaps');

var customOpts = {entries : [ "src/*.html", "src/*.ts" ], debug : true};

var b = watchify(browserify({
    basedir : ".",
    debug : true,
    entries : [ "src/main.ts", "src/simulcast.ts" ],
    cache : {},
    packageCache : {},
}).plugin(tsify));

gulp.task('js', bundle); // so you can run `gulp js` to build the file
b.on('update', bundle); // on any dep update, runs the bundler
b.on('log', fancy_log.info); // output build logs to terminal

gulp.task("copy-html", function() { return gulp.src(customOpts.entries).pipe(gulp.dest("dist")); });
gulp.task('connect', function() { connect.server({root : 'dist', livereload : true, port : 3000}); });

gulp.task('default', gulp.series('js', 'copy-html', 'connect'));

function bundle()
{
    return b
        .bundle()
        // log errors if they happen
        .on('error', fancy_log.error.bind(fancy_log, 'Browserify Error'))
        .pipe(source('bundle.js'))
        // optional, remove if you don't need to buffer file contents
        .pipe(buffer())
        // optional, remove if you dont want sourcemaps
        .pipe(sourcemaps.init({loadMaps : true})) // loads map from browserify file
                                                  // Add transformation tasks to the pipeline here.
        .pipe(sourcemaps.write('./')) // writes .map file
        .pipe(gulp.dest('./dist'));
}
