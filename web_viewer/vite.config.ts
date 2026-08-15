import { defineConfig } from 'vite';
import path from 'path';

export default defineConfig({
  root: path.resolve(__dirname, '..'),
  server: {
    host: true,
    port: 5173,
  },
  build: {
    outDir: path.resolve(__dirname, 'dist'),
  },
});