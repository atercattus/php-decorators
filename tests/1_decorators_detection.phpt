--TEST--
Decorators: decorators detection
--SKIPIF--
<?php require_once(__DIR__.'/skipif.inc'); ?>
--FILE--
<?php
$code =<<<'END'

// Decor name

#@decor
function a(){}

//@decor
function a(){}

/*@decor*/
function a(){}

/**@decor*/
function a(){}

/*
#@decor
*/
function a(){}

//#@decor
function a(){}

#//@decor
function a(){}

# @decor
function a(){}

#@ decor
function a(){}

#   @       decor
function a(){}

@#decor
function a(){}

#@Foo::bar
function a(){}

#@\decor
function a(){}

#@\Foo\bar
function a(){}

    #@Class::method(42)
    public private protected final abstract static function foo(){}

#@кириллица("utf8 однако")
// decorate the function b() below

/**
 * comment
 */
#@decor
/**
 * comment
 */
function b(){}

// Decor params

# @decor()
function a(){}

# @decor ()
function a(){}

# @decor (   )
function a(){}

# @decor(1)
function a(){}

# @decor(1, 2)
function a(){}

# @decor ('foo')
function a(){}

# @decor(Class::method(), array('x'))
function a(){}

# @decor('#@decor()')
function a(){}

// Multi decors

# @x(X)
# @y
# @z(Z, T)
function a(){}

END;
$out = decorators_preprocessor($code);
$out = array_map('trim', explode("\n", $out));
echo implode("\n", $out),"\n//---\n";
?>
--EXPECTF--
// Decor name


function a(){ return call_user_func_array(decor(function() {}), func_get_args());}

//@decor
function a(){}

/*@decor*/
function a(){}

/**@decor*/
function a(){}

/*
#@decor
*/
function a(){}

//#@decor
function a(){}

#//@decor
function a(){}


function a(){ return call_user_func_array(decor(function() {}), func_get_args());}


function a(){ return call_user_func_array(decor(function() {}), func_get_args());}


function a(){ return call_user_func_array(decor(function() {}), func_get_args());}

@#decor
function a(){}


function a(){ return call_user_func_array(Foo::bar(function() {}), func_get_args());}


function a(){ return call_user_func_array(\decor(function() {}), func_get_args());}


function a(){ return call_user_func_array(\Foo\bar(function() {}), func_get_args());}


public private protected final abstract static function foo(){ return call_user_func_array(Class::method(function() {}, 42), func_get_args());}


// decorate the function b() below

/**
* comment
*/

/**
* comment
*/
function b(){ return call_user_func_array(кириллица(decor(function() {}), "utf8 однако"), func_get_args());}

// Decor params


function a(){ return call_user_func_array(decor(function() {}), func_get_args());}


function a(){ return call_user_func_array(decor(function() {}), func_get_args());}


function a(){ return call_user_func_array(decor(function() {},    ), func_get_args());}


function a(){ return call_user_func_array(decor(function() {}, 1), func_get_args());}


function a(){ return call_user_func_array(decor(function() {}, 1, 2), func_get_args());}


function a(){ return call_user_func_array(decor(function() {}, 'foo'), func_get_args());}


function a(){ return call_user_func_array(decor(function() {}, Class::method(), array('x')), func_get_args());}


function a(){ return call_user_func_array(decor(function() {}, '#@decor()'), func_get_args());}

// Multi decors




function a(){ return call_user_func_array(x(y(z(function() {}, Z, T)), X), func_get_args());}

//---
