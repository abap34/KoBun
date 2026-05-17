console.log("script:start");

const id = setTimeout(function () {
    console.log("timer");
    return 42;
}, 0);

console.log("timer id", id);
console.log("script:end");
