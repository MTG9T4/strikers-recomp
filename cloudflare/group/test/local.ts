// Explicit offline harness. Not imported by the production entry point.
import { createHandler } from '../src/index';
const handler = createHandler(async (_env, node, authorized) => {
  console.log(JSON.stringify({ mock_central: true, node, authorized }));
}, true);
export default { fetch: handler };
