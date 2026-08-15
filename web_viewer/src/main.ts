import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { GLTFLoader } from 'three/examples/jsm/loaders/GLTFLoader.js';

const container = document.getElementById('canvas-container')!;

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x222222);

const camera = new THREE.PerspectiveCamera(45, window.innerWidth / window.innerHeight, 0.01, 1000);
camera.position.set(2, 1.5, 3);

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.setPixelRatio(window.devicePixelRatio);
container.appendChild(renderer.domElement);

const controls = new OrbitControls(camera, renderer.domElement);
controls.target.set(0, 0, 0);
controls.update();

const ambientLight = new THREE.AmbientLight(0xffffff, 0.5);
scene.add(ambientLight);
const directionalLight = new THREE.DirectionalLight(0xffffff, 1.0);
directionalLight.position.set(5, 10, 7);
scene.add(directionalLight);

let currentScene: THREE.Group | null = null;

function disposeScene() {
  if (!currentScene) return;
  currentScene.traverse((child) => {
    if (child instanceof THREE.Mesh) {
      child.geometry.dispose();
      if (Array.isArray(child.material)) {
        child.material.forEach((m) => m.dispose());
      } else {
        child.material.dispose();
      }
    }
  });
  scene.remove(currentScene);
  currentScene = null;
}

function frameCamera(obj: THREE.Object3D) {
  const box = new THREE.Box3().setFromObject(obj);
  const center = box.getCenter(new THREE.Vector3());
  const size = box.getSize(new THREE.Vector3());
  const maxDim = Math.max(size.x, size.y, size.z, 0.01);
  const dist = maxDim * 2.0;
  camera.position.set(center.x + dist * 0.5, center.y + dist * 0.4, center.z + dist);
  controls.target.copy(center);
  controls.update();
}

function setStatus(msg: string) {
  const el = document.getElementById('info');
  if (el) el.textContent = msg;
}

function loadFromServer(path: string) {
  disposeScene();
  setStatus(`loading ${path}...`);
  const loader = new GLTFLoader();
  loader.load(
    `/${path}`,
    (gltf) => {
      currentScene = gltf.scene;
      scene.add(currentScene);
      frameCamera(currentScene);
      setStatus(`loaded ${path}`);
    },
    undefined,
    (error) => {
      console.error('GLTF load error:', error);
      setStatus(`error: ${error instanceof Error ? error.message : 'unknown error'}`);
    }
  );
}

function loadFromFiles(files: FileList) {
  disposeScene();
  const fileMap = new Map<string, File>();
  let gltfFile: File | null = null;
  for (let i = 0; i < files.length; i++) {
    const f = files[i];
    fileMap.set(f.name, f);
    if (f.name.endsWith('.gltf') || f.name.endsWith('.glb')) {
      gltfFile = f;
    }
  }
  if (!gltfFile) {
    setStatus('error: no .gltf or .glb file selected');
    return;
  }
  const blobUrlMap = new Map<string, string>();
  for (const [name, f] of fileMap) {
    blobUrlMap.set(name, URL.createObjectURL(f));
  }
  const manager = new THREE.LoadingManager();
  manager.setURLModifier((url: string) => {
    const filename = url.split('/').pop() || url;
    const mapped = blobUrlMap.get(filename);
    if (mapped) return mapped;
    const gltfFilename = gltfFile.name;
    const base = gltfFilename.substring(0, gltfFilename.lastIndexOf('/') + 1);
    const prefixed = base + filename;
    const mapped2 = blobUrlMap.get(prefixed);
    if (mapped2) return mapped2;
    return url;
  });
  const loader = new GLTFLoader(manager);
  const gltfBlobUrl = URL.createObjectURL(gltfFile);
  setStatus(`loading ${gltfFile.name}...`);
  loader.load(
    gltfBlobUrl,
    (gltf) => {
      currentScene = gltf.scene;
      scene.add(currentScene);
      frameCamera(currentScene);
      setStatus(`loaded ${gltfFile!.name}`);
    },
    undefined,
    (error) => {
      console.error('GLTF load error:', error);
      setStatus(`error: ${error instanceof Error ? error.message : 'unknown error'}`);
    }
  );
}

const select = document.getElementById('scene-select') as HTMLSelectElement;
select.addEventListener('change', () => {
  loadFromServer(select.value);
});

const fileInput = document.getElementById('file-input') as HTMLInputElement;
fileInput.addEventListener('change', () => {
  if (fileInput.files && fileInput.files.length > 0) {
    loadFromFiles(fileInput.files);
  }
});

loadFromServer(select.value);

function animate() {
  requestAnimationFrame(animate);
  controls.update();
  renderer.render(scene, camera);
}
animate();

window.addEventListener('resize', () => {
  camera.aspect = window.innerWidth / window.innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(window.innerWidth, window.innerHeight);
});