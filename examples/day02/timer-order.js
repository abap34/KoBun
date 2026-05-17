setTimeout(function () {
    console.log("late");
}, 30);

setTimeout(function () {
    console.log("early");
}, 0);

console.log("script:end");
