// Portable local-only adapter for native integration when workerd cannot launch.
// This is SQLite + the real handler; it is not evidence of D1/workerd execution.
import { DatabaseSync } from 'node:sqlite';
import { readFile } from 'node:fs/promises';
export async function database(path=':memory:') {
  const db=new DatabaseSync(path);
  db.exec(await readFile(new URL('../migrations/0001_group.sql',import.meta.url),'utf8'));
  class Statement {
    constructor(q,args=[]){this.q=q;this.args=args;}
    bind(...args){return new Statement(this.q,args);}
    async first(){return db.prepare(this.q).get(...this.args)??null;}
    async all(){return {results:db.prepare(this.q).all(...this.args)};}
    async run(){return db.prepare(this.q).run(...this.args);}
  }
  // Serialize batches because Node's async functions yield between statements.
  let tail=Promise.resolve();
  return {prepare:q=>new Statement(q),batch(statements){const work=tail.then(async()=>{
    db.exec('BEGIN');try{const results=[];for(const s of statements)results.push(db.prepare(s.q).run(...s.args));db.exec('COMMIT');return results;}catch(e){db.exec('ROLLBACK');throw e;}
  });tail=work.catch(()=>{});return work;}};
}
