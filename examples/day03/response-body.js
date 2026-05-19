const res = new Response("hello");
console.log("body", res.body);

const empty = new Response();
console.log("default body empty", empty.body === "");
