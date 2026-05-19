const ok = new Response("ok");
console.log("default status", ok.status);

const notFound = new Response("missing", { status: 404 });
console.log("custom status", notFound.status);
